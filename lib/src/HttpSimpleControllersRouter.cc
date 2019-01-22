/**
 *
 *  HttpSimpleControllersRouter.cc
 *  An Tao
 *  
 *  Copyright 2018, An Tao.  All rights reserved.
 *  https://github.com/an-tao/drogon
 *  Use of this source code is governed by a MIT license
 *  that can be found in the License file.
 *
 *  Drogon
 *
 */

#include "HttpSimpleControllersRouter.h"
#include "HttpAppFrameworkImpl.h"

using namespace drogon;

void HttpSimpleControllersRouter::registerHttpSimpleController(const std::string &pathName,
                                                               const std::string &ctrlName,
                                                               const std::vector<any> &filtersAndMethods)
{
    assert(!pathName.empty());
    assert(!ctrlName.empty());

    std::string path(pathName);
    std::transform(pathName.begin(), pathName.end(), path.begin(), tolower);
    std::lock_guard<std::mutex> guard(_simpCtrlMutex);
    std::vector<HttpMethod> validMethods;
    std::vector<std::string> filters;
    for (auto const &filterOrMethod : filtersAndMethods)
    {
        if (filterOrMethod.type() == typeid(std::string))
        {
            filters.push_back(*any_cast<std::string>(&filterOrMethod));
        }
        else if (filterOrMethod.type() == typeid(const char *))
        {
            filters.push_back(*any_cast<const char *>(&filterOrMethod));
        }
        else if (filterOrMethod.type() == typeid(HttpMethod))
        {
            validMethods.push_back(*any_cast<HttpMethod>(&filterOrMethod));
        }
        else
        {
            std::cerr << "Invalid controller constraint type:" << filterOrMethod.type().name() << std::endl;
            LOG_ERROR << "Invalid controller constraint type";
            exit(1);
        }
    }
    auto &iterm = _simpCtrlMap[path];
    iterm.controllerName = ctrlName;
    iterm.filtersName = filters;
    iterm._validMethodsFlags.clear(); //There may be old data, first clear
    if (validMethods.size() > 0)
    {
        iterm._validMethodsFlags.resize(Invalid, 0);
        for (auto const &method : validMethods)
        {
            iterm._validMethodsFlags[method] = 1;
        }
    }
}

void HttpSimpleControllersRouter::route(const HttpRequestImplPtr &req,
                                        std::function<void(const HttpResponsePtr &)> &&callback,
                                        bool needSetJsessionid,
                                        std::string &&sessionId)
{
    std::string pathLower(req->path().length(), 0);
    std::transform(req->path().begin(), req->path().end(), pathLower.begin(), tolower);

    if (_simpCtrlMap.find(pathLower) != _simpCtrlMap.end())
    {
        auto &ctrlInfo = _simpCtrlMap[pathLower];
        if (!ctrlInfo._validMethodsFlags.empty())
        {
            assert(ctrlInfo._validMethodsFlags.size() > req->method());
            if (ctrlInfo._validMethodsFlags[req->method()] == 0)
            {
                //Invalid Http Method
                auto res = drogon::HttpResponse::newHttpResponse();
                res->setStatusCode(k405MethodNotAllowed);
                callback(res);
                return;
            }
        }
        auto &filters = ctrlInfo.filtersName;
        if (!filters.empty())
        {
            auto sessionIdPtr = std::make_shared<std::string>(std::move(sessionId));
            auto callbackPtr = std::make_shared<std::function<void(const HttpResponsePtr &)>>(std::move(callback));
            _appImpl.doFilters(filters, req, callbackPtr, needSetJsessionid, sessionIdPtr, [=, pathLower = std::move(pathLower)]() mutable {
                doControllerHandler(std::move(pathLower), req, std::move(*callbackPtr), needSetJsessionid, std::move(*sessionIdPtr));
            });
        }
        else
        {
            doControllerHandler(std::move(pathLower), req, std::move(callback), needSetJsessionid, std::move(sessionId));
        }
        return;
    }
    _httpCtrlsRouter.route(req, std::move(callback), needSetJsessionid, std::move(sessionId));
}

void HttpSimpleControllersRouter::doControllerHandler(std::string &&pathLower,
                                                      const HttpRequestImplPtr &req,
                                                      std::function<void(const HttpResponsePtr &)> &&callback,
                                                      bool needSetJsessionid,
                                                      std::string &&sessionId)
{
    auto &ctrlItem = _simpCtrlMap[pathLower];
    const std::string &ctrlName = ctrlItem.controllerName;
    std::shared_ptr<HttpSimpleControllerBase> controller;
    HttpResponsePtr responsePtr;
    {
        //maybe update controller,so we use lock_guard to protect;
        std::lock_guard<std::mutex> guard(ctrlItem._mutex);
        controller = ctrlItem.controller;
        responsePtr = ctrlItem.responsePtr;
        if (!controller)
        {
            auto _object = std::shared_ptr<DrObjectBase>(DrClassMap::newObject(ctrlName));
            controller = std::dynamic_pointer_cast<HttpSimpleControllerBase>(_object);
            ctrlItem.controller = controller;
        }
    }

    if (controller)
    {
        if (responsePtr && (responsePtr->expiredTime() == 0 || (trantor::Date::now() < responsePtr->createDate().after(responsePtr->expiredTime()))))
        {
            //use cached response!
            LOG_TRACE << "Use cached response";
            if (!needSetJsessionid)
                callback(responsePtr);
            else
            {
                //make a copy response;
                auto newResp = std::make_shared<HttpResponseImpl>(*std::dynamic_pointer_cast<HttpResponseImpl>(responsePtr));
                newResp->setExpiredTime(-1); //make it temporary
                newResp->addCookie("JSESSIONID", sessionId);
                callback(newResp);
            }
            return;
        }
        else
        {
            controller->asyncHandleHttpRequest(req, [=, callback = std::move(callback), pathLower = std::move(pathLower), sessionId = std::move(sessionId)](const HttpResponsePtr &resp) {
                auto newResp = resp;
                if (resp->expiredTime() >= 0)
                {
                    //cache the response;
                    std::dynamic_pointer_cast<HttpResponseImpl>(resp)->makeHeaderString();
                    {
                        auto &item = _simpCtrlMap[pathLower];
                        std::lock_guard<std::mutex> guard(item._mutex);
                        item.responsePtr = resp;
                    }
                }
                if (needSetJsessionid)
                {
                    if (resp->expiredTime() >= 0)
                    {
                        //make a copy
                        newResp = std::make_shared<HttpResponseImpl>(*std::dynamic_pointer_cast<HttpResponseImpl>(resp));
                        newResp->setExpiredTime(-1); //make it temporary
                    }
                    newResp->addCookie("JSESSIONID", sessionId);
                }
                callback(newResp);
            });
        }

        return;
    }
    else
    {
        LOG_ERROR << "can't find controller " << ctrlName;
        auto res = drogon::HttpResponse::newNotFoundResponse();
        if (needSetJsessionid)
            res->addCookie("JSESSIONID", sessionId);

        callback(res);
    }
}