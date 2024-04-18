// #include "LocalDiscovery.hpp"
#include <up-client-vsomeip-cpp/routing/SomeipHandler.hpp>
#include <up-client-vsomeip-cpp/utils/AsioThreadPool.hpp>
#include <up-cpp/uuid/factory/Uuidv8Factory.h>
#include <up-cpp/uuid/serializer/UuidSerializer.h>
#include <up-client-vsomeip-cpp/routing/MiscConfig.hpp>
#include <sstream>

using namespace uprotocol::utransport;
using namespace uprotocol::v1;

constexpr uint16_t MIN_SUBSCRIBER_COUNT  = 1U;

/**
 *  @brief     @see @ref SomeipHandler::SomeipHandler()
 *
 *  @param[in] someIpLibInterface @see @ref SomeipHandler::SomeipHandler()
 *  @param[in] someIpRouterLibInterface @see @ref SomeipHandler::SomeipHandler()
 *  @param[in] someIpRouterHandlerType @see @ref SomeipHandler::SomeipHandler()
 *  @param[in] serviceIdentifier @see @ref SomeipHandler::SomeipHandler()
 *  @param[in] instanceIdentifier @see @ref SomeipHandler::SomeipHandler()
 *  @param[in] qPriorityLevels @see @ref SomeipHandler::SomeipHandler()
 */
SomeipHandler::SomeipHandler(
    SomeipInterface &someIpLibInterface,
    SomeipRouterInterface &someIpRouterLibInterface,
    HandlerType const someIpRouterHandlerType,
    UEntity const &uEntityInfo,
    UAuthority const &uAuthorityInfo,
    instance_t const instanceIdentifier,
    uint16_t qPriorityLevels) : someIpInterface_(someIpLibInterface),
                                routerInterface_(someIpRouterLibInterface),
                                type_(someIpRouterHandlerType),
                                instanceId_(instanceIdentifier),
                                uEntityData_(uEntityInfo),
                                uAuthorityData_(uAuthorityInfo),
                                queue_(qPriorityLevels),
                                isServiceAvailable_(false) {

    LogDebug("{}", __FUNCTION__);
    instanceId_ = static_cast<uint16_t>(INSTANCE_ID_PER_SPEC);
    service_t serviceId = static_cast<service_t>(uEntityData_.id());

    registerMessageHandler();
    if (type_ == HandlerType::Client) {
        if (routerInterface_.isStateRegistered()) {
            LogInfo("{} State registered", __FUNCTION__);
            someIpInterface_.registerAvailabilityHandler(
                serviceId,
                instanceId_,
                std::bind(
                    &SomeipHandler::onAvailability,
                    this,
                    std::placeholders::_1,
                    std::placeholders::_2,
                    std::placeholders::_3));
            someIpInterface_.requestService(serviceId, instanceId_);
        }
    } else if (type_ == HandlerType::Server) {
        LogDebug("{} Offer Service for serviceID : 0x[{:x}]", __FUNCTION__, serviceId);
        someIpInterface_.offerService(serviceId, instanceId_);
    } else {
        LogErr("{} Unknown handler type", __FUNCTION__);
        return;
    }
}

/**
  * @brief @see @ref SomeipHandler::offerEvent()
  *
  * @param[in] eventGroupSetPtr @see @ref SomeipHandler::offerEvent()
  */
void SomeipHandler::offerEvent(std::shared_ptr<std::set<eventgroup_t>> eventGroupSetPtr) {
    service_t serviceId = static_cast<service_t>(uEntityData_.id());
    // Iterate all event groups associated with service ID and offer event
    for (const auto &eventGroupId : *eventGroupSetPtr) {
        LogTrace("{} - Offer Event EID[0x{:x}] for SID[0x{:x}] ", __FUNCTION__, eventGroupId, serviceId);
        std::stringstream ss;
        ss << std::hex << eventGroupId;

        someIpInterface_.registerSubscriptionHandler(
            serviceId,
            INSTANCE_ID_PER_SPEC,
            eventGroupId,
            std::bind(
                &SomeipHandler::onSubscription,
                this,
                std::placeholders::_1,
                std::placeholders::_2,
                ss.str(),
                std::placeholders::_4));

         someIpInterface_.offerEvent(serviceId,
                                INSTANCE_ID_PER_SPEC,
                                eventGroupId,
                                *eventGroupSetPtr,
                                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);
    }
}

/**
  * @brief @see @ref SomeipHandler::queueOfferUResource
  *
  * @param @see @ref SomeipHandler::queueOfferUResource
  */
void SomeipHandler::queueOfferUResource(std::shared_ptr<UUri> uriPtr) {
    LogDebug("{}", __FUNCTION__);
    if (running_) {
        if(! postMessageToQueue(HandlerMsgType::OfferUResource, 0UL, uriPtr)) {
            LogErr("{} Failed to post message to queue", __FUNCTION__);
        }
    } else {
        LogWarn("%s Current handler is not running in any thread.", __FUNCTION__);
    }
}

/**
 *  @brief     @see @ref SomeipHandler::queueOutboundMsg()
 *
 *  @param[in] msg @see @ref SomeipHandler::queueOutboundMsg()
 */
void SomeipHandler::queueOutboundMsg(const UMessage &msg) {
    LogDebug("{}", __FUNCTION__);
    if (running_) {
        /*
            TODO: Need to check how to optimize/remove this copy. There are 2 ways of fixing this.
            1) Change the UTransport API's as taking shared pointers instead of references
            2) Remove Queing of messages.
        */
        auto messagePtr = std::make_shared<UMessage>(msg);
        if(! postMessageToQueue(HandlerMsgType::Outbound, 0UL, messagePtr)) {
            LogErr("{} Failed to post message to queue", __FUNCTION__);
        }
    } else {
        LogWarn("%s Current handler is not running in any thread.", __FUNCTION__);
    }
}

/**
 *  @brief     @see @ref SomeipHandler::onMessage()
 *
 *  @param[in] msg @see @ref SomeipHandler::onMessage()
 */
void SomeipHandler::onMessage(std::shared_ptr<message> const &msg) {
    LogDebug("{}", __FUNCTION__);
    if (!running_) {
        LogWarn("{} Current handler is not running in any thread.", __FUNCTION__);
        if(!startThread()) {
            LogErr("{} Failed to start thread", __FUNCTION__);
        }
    } else {
        LogDebug("{} Handler already has a thread ", __FUNCTION__);
    }
    if(! postMessageToQueue(HandlerMsgType::Inbound, 0UL, msg)) {
        LogErr("{} Failed to post message to queue", __FUNCTION__);
    }
}

/**
 *  @brief     @see @ref SomeipHandler::onAvailability()
 *
 *  @param[in] service @see @ref SomeipHandler::onAvailability()
 *  @param[in] instance @see @ref SomeipHandler::onAvailability()
 *  @param[in] isAvailable @see @ref SomeipHandler::onAvailability()
 */
void SomeipHandler::onAvailability(service_t service, instance_t instance, const bool isAvailable) {
    const char *str = isAvailable ? "available" : "unavailable";
    if(isAvailable) {
        isServiceAvailable_.setValueAndNotify(isAvailable);
    }
    LogInfo("{} service[0x{:x}] instance[0x{:x}] {}", __FUNCTION__, service, instance, str);
}

/**
 *  @brief     @see @ref SomeipHandler::onSubscription()
 *
 *  @param[in] client @ref @see SomeipHandler::onSubscription()
 *  @param[in] secClient @see @ref SomeipHandler::onSubscription()
 *  @param[in] eventIdStr @see @ref SomeipHandler::onSubscription()
 *  @param[in] isSubscribed @see @ref SomeipHandler::onSubscription()
 *
 *  @return    @see @ref SomeipHandler::onSubscription()
 */
bool SomeipHandler::onSubscription(
    client_t const client,
    secClientType const *const secClient,
    std::string const &eventIdStr,
    bool const isSubscribed) noexcept {

    std::ignore = secClient;
    bool bRet = false;

    LogInfo("{} client[0x{:x}] . eventIdStr[{}], serviceid[{:x}], isSubscribed[{}]",
        __FUNCTION__, client, eventIdStr.c_str(), uEntityData_.id(), isSubscribed);

    std::stringstream ss;
    ss << std::hex << eventIdStr;
    eventgroup_t eventgroup;
    ss >> eventgroup;

    try {
        std::shared_ptr<subscriptionStatus> const subStatus = std::make_shared<subscriptionStatus>();
        subStatus->isSubscribed = isSubscribed;
        subStatus->eventgroup = eventgroup;

        LogInfo("{} - eventgroup[0x{:x}]", __FUNCTION__, eventgroup);
        if(! postMessageToQueue(HandlerMsgType::InboundSubscription, 0UL, subStatus)) {
            LogErr("{} Failed to post message to queue", __FUNCTION__);
            bRet = false;
        }
        bRet = true;
    }
    catch(const std::exception& e) {
        LogErr("{} Exception caught: {}", __FUNCTION__, e.what());
        bRet = false;
    }
    return bRet;
}

/**
 *  @brief     @see @ref SomeipHandler::onSubscriptionStatus()
 *
 *  @param[in] service @ref @see SomeipHandler::onSubscriptionStatus()
 *  @param[in] instance @see @ref SomeipHandler::onSubscriptionStatus()
 *  @param[in] eventGroup @see @ref SomeipHandler::onSubscriptionStatus()
 *  @param[in] event @see @ref SomeipHandler::onSubscriptionStatus()
 *  @param[in] status @see @ref SomeipHandler::onSubscriptionStatus()
 */
void SomeipHandler::onSubscriptionStatus(
    service_t const service,
    instance_t const instance,
    eventgroup_t const eventGroup,
    event_t const event,
    uint16_t const status) {

    static_cast<void>(event);
    LogInfo("{} service[0x{:x}] instance[0x{:x}] eventgrp[0x{:x}] status[{}]",
        __FUNCTION__, service, instance, eventGroup, status);

    bool const isSubscribed = (status == 0U) ? true : false;

    try {
        std::shared_ptr<subscriptionStatus> const subStatus = std::make_shared<subscriptionStatus>();
        subStatus->isSubscribed = isSubscribed;
        subStatus->eventgroup = eventGroup;
        if(! postMessageToQueue(HandlerMsgType::InboundSubscriptionAck, 0UL, subStatus)) {
            LogErr("{} Failed to post message to queue", __FUNCTION__);
        }
    }
    catch(const std::exception& e) {
        LogErr("{} Exception caught: {}", __FUNCTION__, e.what());
    }
}

/**
 *  @brief @see @ref SomeipHandler::quit()
 */
void SomeipHandler::quit() {
    if (running_) {
        stopThread();
        qCondVar_.notify_one();  // wake up the worker
        flush();
    }
}

/**
 *  @brief @see @ref SomeipHandler::flush()
 */
void SomeipHandler::flush() {
    std::lock_guard<std::mutex> const lck(qMutex_);
    for (auto &it: queue_) {
        while (!it.empty()) {
            it.pop();
        }
    }
    queue_.clear();
}

/**
 *  @brief  @see @ref SomeipHandler::startThread()
 *
 *  @return @see @ref SomeipHandler::startThread()
 */
bool SomeipHandler::startThread() {
    LogTrace("{}", __FUNCTION__);
    if (! running_) {
        LogTrace("{} Starting new handler thread",__FUNCTION__);
        std::function<void()> const func = std::bind(&SomeipHandler::executor, this);
        AsioThreadPool::getInstance().post(func);
        running_ = true;
        return running_;
    }
    return false;
}

/**
 *  @brief  @see @ref SomeipHandler::stopThread()
 *
 *  @return @see @ref SomeipHandler::stopThread()
 */
void SomeipHandler::stopThread() noexcept {
    LogTrace("{}", __FUNCTION__);
    if (running_) {
        LogTrace("{} Stopping handler thread", __FUNCTION__);
        postMessageToQueue(HandlerMsgType::Stop, 0UL, nullptr);
        running_ = false;
    }
}

/**
 *  @brief     @see @ref SomeipHandler::postMessageToQueue()
 *
 *  @param[in] someIpRHandlerType @see @ref SomeipHandler::postMessageToQueue()
 *  @param[in] data @see @ref SomeipHandler::postMessageToQueue()
 *  @param[in] ptr @see @ref SomeipHandler::postMessageToQueue()
 *  @param[in] priority @see @ref SomeipHandler::postMessageToQueue()
 *
 *  @return    @see @ref SomeipHandler::postMessageToQueue()
 */
bool SomeipHandler::postMessageToQueue(
    HandlerMsgType someIpRHandlerType,
    unsigned long data,
    std::shared_ptr<void> ptr,
    uint16_t const priority) {

    bool isOk{false};
    if (running_){
        if (priority < queue_.size()) {
            {
                std::lock_guard<std::mutex> const lck(qMutex_);
                queue_[priority].push(std::make_unique<QItem>(someIpRHandlerType, data, ptr));
            }
            isOk = true;
            qCondVar_.notify_one(); // wake up the worker
        }
    } else {
        LogWarn("Current handler is not running in any thread.");
    }
    return isOk;
}

/**
 *  @brief @see @ref SomeipHandler::executor()
 */
void SomeipHandler::executor() {
    LogDebug("{}", __FUNCTION__);
    while (true) {
        std::unique_lock<std::mutex> lck(qMutex_);
        //TODO need to replace this wait with wait_for, to avoid indefinite wait(allow timeout)
        qCondVar_.wait(lck, [this]() -> bool {
            return !queue_.empty();
        });

        for (auto iter = queue_.rbegin(); iter != queue_.rend(); iter++) {
            while (!iter->empty()) {
                auto item = std::move(iter->front());
                iter->pop();
                lck.unlock();
                processMessage(std::move(item));
                lck.lock();
            }
        }
    }
    stopThread();
}

/**
 *  @brief     @see @ref SomeipHandler::processMessage()
 *
 *  @param[in] item @see @ref SomeipHandler::processMessage()
 */
void SomeipHandler::processMessage(std::unique_ptr<QItem>const item) {
    LogDebug("{} msgReceivedp[{}]", __FUNCTION__, static_cast<int>(item->getMsgType()));
    switch (item->getMsgType()) {
        case HandlerMsgType::Outbound: {
            handleOutboundMsg(std::static_pointer_cast<UMessage>(item->getPtr()));
        }
        break;
        case HandlerMsgType::Inbound: {
            handleInboundMsg(std::static_pointer_cast<message>(item->getPtr()));
        }
        break;
        case HandlerMsgType::InboundSubscriptionAck: {
            handleInboundSubscriptionAck(std::static_pointer_cast<subscriptionStatus>(item->getPtr()));
        }
        break;
        case HandlerMsgType::InboundSubscription: {
            handleInboundSubscription(std::static_pointer_cast<subscriptionStatus>(item->getPtr()));
        }
        break;
        case HandlerMsgType::Stop: {
        }
        break;
        case HandlerMsgType::OfferUResource: {
            handleOfferUResource(std::static_pointer_cast<UUri>(item->getPtr()));
        }
        break;
        default: {
            LogErr("Unknown message type");
        }
        break;
    }
}

/**
 *  @brief     @see @ref SomeipHandler::handleOutboundMsg()
 *
 *  @param[in] uMsg @see @ref SomeipHandler::handleOutboundMsg()
 */
void SomeipHandler::handleOutboundMsg(std::shared_ptr<UMessage> const uMsg) {
    LogTrace("{}", __FUNCTION__);

    switch (uMsg->attributes().type()) {
        case UMessageType::UMESSAGE_TYPE_PUBLISH: {
            handleOutboundNotification(uMsg);
            break;
        }
        case UMessageType::UMESSAGE_TYPE_REQUEST: {
            handleOutboundRequest(uMsg);
            break;
        }
        case UMessageType::UMESSAGE_TYPE_RESPONSE: {
            handleOutboundResponse(uMsg);
            break;
        }
        default: {
            LogErr("{} Unknown message type [{}]", __FUNCTION__, static_cast<uint16_t>(uMsg->attributes().type()));
            break;
        }
    }
}

/**
 *  @brief     @see @ref SomeipHandler::handleInboundMsg()
 *
 *  @param[in] msg @see @ref SomeipHandler::handleInboundMsg()
 */
void SomeipHandler::handleInboundMsg(std::shared_ptr<message> const msg) {
    LogDebug("{}", __FUNCTION__);
    message_type_e const messageType = msg->get_message_type();
    switch (messageType) {
        case message_type_e::MT_REQUEST: {
            handleInboundRequest(msg);
        }
        break;
        case message_type_e::MT_RESPONSE: {
            handleInboundResponse(msg);
        }
        break;
        case message_type_e::MT_NOTIFICATION: {
            handleInboundNotification(msg);
        }
        break;
        default: {
            LogErr("{} Unknown message type", __FUNCTION__);
        }
        break;
    }

}

/**
 *  @brief     @see @ref SomeipHandler::handleInboundNotification()
 *
 *  @param[in] sMsg @see @ref SomeipHandler::handleInboundNotification()
 */
void SomeipHandler::handleInboundNotification(std::shared_ptr<message> sMsg) {
    LogDebug("{}", __FUNCTION__);
    method_t const method = sMsg->get_method(); //someip method is same as eventgroup for notifications
    auto const subscriptionEntry = subscriptionsForRemoteServices_.find(method);
    if (subscriptionEntry != subscriptionsForRemoteServices_.end()) {

        std::shared_ptr<UMessage> const uMsg = routerInterface_.getMessageTranslator()
                                           .translateSomeipToUMsgForNotification(sMsg,
                                                                                uEntityData_,
                                                                                uAuthorityData_,
                                                                                subscriptionEntry->second->getUResource());
        (void)routerInterface_.routeInboundMsg(*uMsg);

    } else {
        LogWarn("{} received a non-requested notification for service[0x{:x}] method[0x{:x}] ",
            __FUNCTION__, uEntityData_.id(), method);
    }
}

/**
 *  @brief     @see @ref SomeipHandler::handleInboundRequest()
 *
 *  @param[in] sMsg @see @ref SomeipHandler::handleInboundRequest()
 */
void SomeipHandler::handleInboundRequest(std::shared_ptr<message> sMsg) {
    LogDebug("{}", __FUNCTION__);
    std::shared_ptr<ResourceInformation> resourceInfo = offeredResources_.at(sMsg->get_method());
    if(resourceInfo != nullptr) {
        const UResource& uResource     = resourceInfo->getUResource();
        std::shared_ptr<UMessage> uMsg = routerInterface_.getMessageTranslator()
                                        .translateSomeipToUMsgForRequest(sMsg, uEntityData_, uResource);
        std::string strUUID            = uprotocol::uuid::UuidSerializer::serializeToString(uMsg->attributes().id());
        (void)uuidToSomeipRequestLookup_.insert({strUUID, sMsg});
        (void)routerInterface_.routeInboundMsg(*uMsg);
    }

}

/**
 *  @brief     @see @ref SomeipHandler::handleInboundResponse()
 *
 *  @param[in] sMsg @see @ref SomeipHandler::handleInboundResponse()
 */
void SomeipHandler::handleInboundResponse(std::shared_ptr<message> sMsg) {
    LogDebug("{}", __FUNCTION__);
    request_t requestId = sMsg->get_request();
    LogDebug("{} Inbound response for requestId[0x{:x}] ", __FUNCTION__, requestId);

    auto search = someipReqIdToUTransportRequestLookup_.find(requestId);
    if (search != someipReqIdToUTransportRequestLookup_.end()) {
        std::shared_ptr<UMessage> const requestUMsg = search->second;
        std::shared_ptr<UMessage> const responseUMsg = routerInterface_.getMessageTranslator()
                                            .translateSomeipToUMsgForResponse(sMsg, requestUMsg);
        static_cast<void>(routerInterface_.routeInboundMsg(*responseUMsg));
        static_cast<void>(someipReqIdToUTransportRequestLookup_.erase(requestId));
    } else {
        LogErr("{} Unable to process inbound response requestId[0x{:x}].", __FUNCTION__, requestId);
    }

}

/**
 *  @brief     @see @ref SomeipHandler::handleInboundSubscription()
 *
 *  @param[in] subStatus @see @ref SomeipHandler::handleInboundSubscription()
 */
void SomeipHandler::handleInboundSubscription(std::shared_ptr<subscriptionStatus> const subStatus) {
    LogDebug("{}", __FUNCTION__);

    UResourceId_t resourceId = static_cast<UResourceId_t>(subStatus->eventgroup);
    if (subStatus->isSubscribed) {
        LogDebug("{} Inbound subscription request received for event[0x{:x}]", __FUNCTION__, subStatus->eventgroup);
        auto const searchResult = offeredResources_.find(resourceId);
        if ( searchResult != offeredResources_.end()) {
            searchResult->second->addSubscriber();
            //TODO In future may be forwarded to uSubscription.
            //std::shared<UUri> uriPtr = translateSomeipToURIForSubscription(serviceId_, resourceId, searchResult->second);
            //routerInterface_.routeInboundSubscription()
        } else {
            LogErr("{} Received event subscription request for unsupported event[0x{:x}]",__FUNCTION__, subStatus->eventgroup);
        }
    } else {
        LogDebug("{} Inbound unsubscribe request received for event[0x{:x}]", __FUNCTION__, subStatus->eventgroup);
        auto const searchResult = offeredResources_.find(resourceId);
        if (searchResult != offeredResources_.end()) {
            searchResult->second->removeSubscriber();
        } else {
            LogErr("{} Received event unsubscription request for unsupported event[0x{:x}]",__FUNCTION__, subStatus->eventgroup);
        }
    }

}

/**
 *  @brief     @see @ref SomeipHandler::handleInboundSubscriptionAck()
 *
 *  @param[in] subStatus @see @ref SomeipHandler::handleInboundSubscriptionAck()
 */
void SomeipHandler::handleInboundSubscriptionAck(std::shared_ptr<subscriptionStatus> const subStatus) {
    LogDebug("{}", __FUNCTION__);

    eventgroup_t eventGroup = subStatus->eventgroup;
    auto eventGrpEntry = subscriptionsForRemoteServices_.find(eventGroup);

    if (eventGrpEntry != subscriptionsForRemoteServices_.end()) {
        LogInfo("{} Subscription ack received for eventgroup[0x{:x}]", __FUNCTION__, eventGroup);
        std::shared_ptr<ResourceInformation> resourceInfo = eventGrpEntry->second;
        std::shared_ptr<UMessage> uMessageSubAck = routerInterface_.getMessageTranslator()
            .translateSomeipToUMsgForSubscriptionAck(uEntityData_,uAuthorityData_, resourceInfo->getUResource());\

        routerInterface_.routeInboundMsg(*uMessageSubAck);

    } else {
        LogWarn("{} received a non-requested subscription ack for eventgroup[0x{:x}] ", __FUNCTION__, eventGroup);
    }

}

/**
 *  @brief     @see @ref SomeipHandler::handleOutboundNotification()
 *
 *  @param[in] uMsg @see @ref SomeipHandler::handleOutboundNotification()
 */
void SomeipHandler::handleOutboundNotification(std::shared_ptr<UMessage> const uMsgPtr) {
    LogDebug("{}", __FUNCTION__);
    UMessage const &uMsg = *uMsgPtr;
    service_t serviceId = static_cast<service_t>(uEntityData_.id());
    std::vector<uint8_t>  payloadData = buildSomeipPayloadFromUMessage(uMsg);
    auto const its_payload = someIpInterface_.createPayload();
    its_payload->set_data(payloadData);

    if (doesInboundSubscriptionExist(static_cast<eventgroup_t>(uMsg.attributes().source().resource().id()))) {
        someIpInterface_.notify(serviceId,
                                INSTANCE_ID_PER_SPEC,
                                static_cast<uint16_t>(uMsg.attributes().source().resource().id()),
                                its_payload);
        LogInfo("{} Outbound notification is published for service[0x{:x}] eventgroup[0x{:x}]",
                __FUNCTION__, serviceId, uMsg.attributes().source().resource().id());
    } else {
        LogWarn("{} Outbound notification is not published as no subscription found for eventgroup[0x{:x}]",
                __FUNCTION__, uMsg.attributes().source().resource().id());
    }

}

/**
 *  @brief     @see @ref SomeipHandler::handleOutboundRequest()
 *
 *  @param[in] uMsg @see @ref SomeipHandler::handleOutboundRequest()
 */
void SomeipHandler::handleOutboundRequest(std::shared_ptr<UMessage> const uMsgPtr) {
    LogDebug("{}", __FUNCTION__);

    if (uMsgPtr->attributes().sink().entity().id() == UT_SUBSCRIPTION_REQUEST_SINK_URI_UEID) {
        // Subscription request for remote service
        handleSubscriptionRequestForRemoteService(uMsgPtr);
        return;
    }

    std::shared_ptr<message> const someIpRequest =
        routerInterface_.getMessageTranslator().translateUMessageToSomeipMsgForRequest(uMsgPtr);

    bool serviceAvailableStatus = isServiceAvailable_.isReadableWithWait(std::chrono::milliseconds(uMsgPtr->attributes().ttl()));

    if(serviceAvailableStatus) {
        LogInfo("{} Service is available with service[0x{:x}] methodId[0x{:x}]",
            __FUNCTION__,
            uEntityData_.id(),
            uMsgPtr->attributes().sink().resource().id());
        someIpInterface_.send(someIpRequest);
        request_t requestId = someIpRequest->get_request();
        LogInfo("{} New request service[0x{:x}] methodId[0x{:x}] requestId[0x{:x}]",
            __FUNCTION__,
            uEntityData_.id(),
            uMsgPtr->attributes().sink().resource().id(),
            requestId);
        (void)someipReqIdToUTransportRequestLookup_.insert({requestId, uMsgPtr});
    }
    else {
       LogWarn("{} Service is unavailable with service[0x{:x}] methodId[0x{:x}]",
            __FUNCTION__,
            uEntityData_.id(),
            uMsgPtr->attributes().sink().resource().id());
    }
}

/**
 *  @brief     @see @ref SomeipHandler::handleSubscriptionRequestForRemoteService()
 *
 *  @param[in] UUri @see @ref SomeipHandler::handleSubscriptionRequestForRemoteService()
 */
void SomeipHandler::handleSubscriptionRequestForRemoteService(std::shared_ptr<uprotocol::utransport::UMessage> const uMsgPtr) {
    LogDebug("{}", __FUNCTION__);

    if ((routerInterface_.isStateRegistered())) {
        LogInfo("{} State registered and service available", __FUNCTION__);
        eventgroup_t eventGroup = static_cast<eventgroup_t>(uMsgPtr->attributes().source().resource().id());
        std::shared_ptr<ResourceInformation> resource =
            std::make_shared<ResourceInformation>(uMsgPtr->attributes().source().resource());

        if (! doesSubscriptionForRemoteServiceExist(eventGroup)) {
            LogInfo("{} Adding new eventgroup[0x{:x}] subscription entry", __FUNCTION__, eventGroup);
            event_t const event = eventGroup; // event will be same as eventGroup
            registerSubscriptionStatusHandler(eventGroup);
            addSubscriptionForRemoteService(eventGroup, resource);
            std::set<eventgroup_t> eventGroups;
            (void)eventGroups.insert(eventGroup);
            service_t serviceId = static_cast<service_t>(uEntityData_.id());
            someIpInterface_.requestEvent(serviceId, instanceId_, event, eventGroups, event_type_e::ET_FIELD);
            someIpInterface_.subscribe(serviceId, instanceId_, eventGroup);
        } else {
            LogInfo("{} eventgroup[{}] subscription entry already exist", __FUNCTION__, eventGroup);
            actOnBehalfOfSubscriptionAck(eventGroup);
        }
    } else {
        LogWarn("{} State not registered or service not available", __FUNCTION__);
    }
}

/**
 *  @brief     @see @ref SomeipHandler::handleOutboundResponse()
 *
 *  @param[in] uMsg @see @ref SomeipHandler::handleOutboundResponse()
 */
void SomeipHandler::handleOutboundResponse(std::shared_ptr<UMessage> const uMsgPtr) {
    LogDebug("{}", __FUNCTION__);
    UMessage const &uMsg = *uMsgPtr;
    std::string strUUID = uprotocol::uuid::UuidSerializer::serializeToString(uMsg.attributes().reqid());
    auto search = uuidToSomeipRequestLookup_.find(strUUID);

    if( search != uuidToSomeipRequestLookup_.end()) {

        auto someipRequestMsg = search->second;
        std::shared_ptr<vsomeip::message> const someipResponseMsg = vsomeip::runtime::get()->create_response(someipRequestMsg);
        auto const responsePayload = vsomeip::runtime::get()->create_payload();
        std::vector<uint8_t>  payloadData = buildSomeipPayloadFromUMessage(uMsg);
        responsePayload->set_data(payloadData);
        someipResponseMsg->set_payload(responsePayload);

        someIpInterface_.send(someipResponseMsg);
        LogInfo("{} Outbound response is sent for service[0x{:x}] methodId[0x{:x}]",
                __FUNCTION__,
                static_cast<service_t>(uEntityData_.id()),
                uMsg.attributes().source().resource().id());

        (void)uuidToSomeipRequestLookup_.erase(strUUID);
    } else {
        LogErr("{} Request not found for UUID[{}]", __FUNCTION__, strUUID);
        return;
    }
}

/**
 *  @brief     @see @ref SomeipHandler::handleOfferUResource()
 *
 *  @param[in] listenerInfo @see @ref SomeipHandler::handleOfferUResource()
 */
void SomeipHandler::handleOfferUResource(std::shared_ptr<UUri> const uriPtr) {
    LogInfo("{}",__FUNCTION__);
    UResourceId_t resourceId     = uriPtr->resource().id();
    std::shared_ptr<ResourceInformation> resourceInformation =
        std::make_shared<ResourceInformation>(uriPtr->resource());

    std::ignore = offeredResources_.insert({resourceId, resourceInformation});

    if (! someIpInterface_.isMethod(static_cast<method_t>(resourceId))) {
        std::shared_ptr<std::set<eventgroup_t>> const eventGroupSetPtr = std::make_shared<std::set<eventgroup_t>>();
        eventgroup_t eventGroup = static_cast<eventgroup_t>(uriPtr->resource().id());
        std::ignore = eventGroupSetPtr->insert(eventGroup);
        offerEvent(eventGroupSetPtr);
    } else {
        LogInfo("{} resource id[0x{:x}] is a method, no need to offer", __FUNCTION__, resourceId);
    }
}

/**
 *  @brief     @see @ref SomeipHandler::registerSubscriptionStatusHandler()
 *
 *  @param[in] eventGroup @see @ref SomeipHandler::registerSubscriptionStatusHandler()
 */
void SomeipHandler::registerSubscriptionStatusHandler(eventgroup_t const eventGroup) {
    LogDebug("{}", __FUNCTION__);
    someIpInterface_.registerSubscriptionStatusHandler(
        static_cast<service_t>(uEntityData_.id()),
        instanceId_,
        eventGroup,
        eventGroup,
        std::bind(
            &SomeipHandler::onSubscriptionStatus,
            this,
            std::placeholders::_1,
            std::placeholders::_2,
            std::placeholders::_3,
            std::placeholders::_4,
            std::placeholders::_5));
}

/**
 *  @brief @see @ref SomeipHandler::registerMessageHandler()
 */
void SomeipHandler::registerMessageHandler() {
    someIpInterface_.registerMessageHandler(static_cast<service_t>(uEntityData_.id()), ANY_INSTANCE, ANY_METHOD,
        std::bind(&SomeipHandler::onMessage, this, std::placeholders::_1));
}

/**
 *  @brief     @see @ref SomeipHandler::doesSubscriptionForRemoteServiceExist()
 *
 *  @param[in] eventGroup @see @ref SomeipHandler::doesSubscriptionForRemoteServiceExist()
 *
 *  @return    @see @ref SomeipHandler::doesSubscriptionForRemoteServiceExist()
 */
bool SomeipHandler::doesSubscriptionForRemoteServiceExist(eventgroup_t const eventGroup) {
    LogDebug("{}", __FUNCTION__);
    auto const search = subscriptionsForRemoteServices_.find(eventGroup);
    if (search != subscriptionsForRemoteServices_.end()) {
        return true;
    }
    return false;
}

/**
 *  @brief     @see @ref SomeipHandler::addSubscriptionForRemoteService()
 *
 *  @param[in] @see @ref SomeipHandler::addSubscriptionForRemoteService()
 */
void SomeipHandler::addSubscriptionForRemoteService(
    UResourceId_t resourceId,
    std::shared_ptr<ResourceInformation> resource) {

    LogDebug("{}", __FUNCTION__);

    auto search = subscriptionsForRemoteServices_.find(resourceId);
    if (search == subscriptionsForRemoteServices_.end()) {
        // Entry does not exist, add a new entry in the map
        resource->addSubscriber();
        subscriptionsForRemoteServices_.insert({resourceId, resource});
    } else {
        search->second->addSubscriber();
    }
}


/**
 *  @brief     @see @ref SomeipHandler::removeSubscriptionForRemoteService()
 *
 *  @param[in] @see @ref SomeipHandler::removeSubscriptionForRemoteService()
 */
void SomeipHandler::removeSubscriptionForRemoteService(UResourceId_t resourceId) {
    LogDebug("{}", __FUNCTION__);
    auto search = subscriptionsForRemoteServices_.find(resourceId);
    if (search != subscriptionsForRemoteServices_.end()) {
        search->second->removeSubscriber();
        if (search->second->getSubscriberCount() == 0) {
            subscriptionsForRemoteServices_.erase(search);
        }
    }

}

/**
 *  @brief     @see @ref SomeipHandler::simulateOutboundSubscriptionAck()
 *
 *  @param[in] eventGroup @see @ref SomeipHandler::simulateOutboundSubscriptionAck()
 */
void SomeipHandler::actOnBehalfOfSubscriptionAck(eventgroup_t const eventGroup) {
    LogDebug("{}", __FUNCTION__);
    onSubscriptionStatus(static_cast<service_t>(uEntityData_.id()), instanceId_, eventGroup, eventGroup, 0U);
}

/**
 *  @brief     @see @ref SomeipHandler::doesInboundSubscriptionExist()
 *
 *  @param[in] eventGroup @see @ref SomeipHandler::doesInboundSubscriptionExist()
 *
 *  @return    @see @ref SomeipHandler::doesInboundSubscriptionExist()
 */
bool SomeipHandler::doesInboundSubscriptionExist(eventgroup_t const eventGroup) {
    LogDebug("{}", __FUNCTION__);
    auto const search = offeredResources_.find(static_cast<UResourceId_t>(eventGroup));
    if (search != offeredResources_.end()) {
        std::shared_ptr<ResourceInformation> resourceInformation = search->second;
        return (resourceInformation->doesSubscriptionExist());
    }
    return false;
}


// TODO probably move this into MessageTranslator, as it is more related to translation
// translateUMessageToSomeipMsgForRequest also uses this functionality
/**
  * @brief @see @ref SomeipHandler::buildSomeipPayloadFromUMessage
  * @param[in] uMessage - @see @ref SomeipHandler::buildSomeipPayloadFromUMessage
  * @return @see @ref SomeipHandler::buildSomeipPayloadFromUMessage
  */
std::vector<uint8_t> SomeipHandler::buildSomeipPayloadFromUMessage(const UMessage &uMessage) {
    std::vector<uint8_t> payloadData;
    for(size_t iCount = 0 ; iCount < uMessage.payload().size(); iCount++) {
        payloadData.push_back(uMessage.payload().data()[iCount]);
    }
    return payloadData;
}
