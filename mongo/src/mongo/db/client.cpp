// client.cpp

/**
*    Copyright (C) 2009 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

/* Client represents a connection to the database (the server-side) and corresponds
   to an open socket (or logical connection if pooling on sockets) from a client.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"

#include <boost/functional/hash.hpp>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exit.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
//Client::initThread中初始化赋值，获取方法参考serviceContext->makeOperationContext(&cc());
thread_local ServiceContext::UniqueClient currentClient;
}  // namespace

//ServiceContext::makeClient中创建client对象
void Client::initThreadIfNotAlready(StringData desc) {
    if (currentClient)
        return;
    initThread(desc);
}

void Client::initThreadIfNotAlready() {
    initThreadIfNotAlready(getThreadName());
}

//初始化一个名称desc线程     //desc代表线程名
void Client::initThread(StringData desc, transport::SessionHandle session) {
    initThread(desc, getGlobalServiceContext(), std::move(session));
}

//设置当前线程名，参考_initAndListen，同时获取当client信息currentClient
void Client::initThread(StringData desc, //desc代表线程名
                        ServiceContext* service,
                        transport::SessionHandle session) {
    invariant(!haveClient());

    std::string fullDesc;
    if (session) { //如果带有session信息，添加上对应的session id
        fullDesc = str::stream() << desc << session->id();
    } else {
        fullDesc = desc.toString();
    }

	//设置线程名
    setThreadName(fullDesc);

    // Create the client obj, attach to thread
    //ServiceContext::makeClient
    currentClient = service->makeClient(fullDesc, std::move(session));
}

void Client::destroy() {
    invariant(haveClient());
    currentClient.reset(nullptr);
}

namespace {
int64_t generateSeed(const std::string& desc) {
    size_t seed = 0;
    boost::hash_combine(seed, Date_t::now().asInt64());
    boost::hash_combine(seed, desc);
    return seed;
}
}  // namespace

//例如ServiceContext::makeClient中构造
Client::Client(std::string desc, ServiceContext* serviceContext, transport::SessionHandle session)
    : _serviceContext(serviceContext),
      _session(std::move(session)),
      _desc(std::move(desc)),
      _threadId(stdx::this_thread::get_id()),
      _connectionId(_session ? _session->id() : 0),
      _prng(generateSeed(_desc)) {}

void Client::reportState(BSONObjBuilder& builder) {
    builder.append("desc", desc());

    std::stringstream ss;
    ss << _threadId;
    builder.append("threadId", ss.str());

    if (_connectionId) {
        builder.appendNumber("connectionId", _connectionId);
    }

    if (hasRemote()) {
        builder.append("client", getRemote().toString());
    }
}

//ServiceStateMachine::_processMessage
//获取一个唯一得UniqueOperationContext   一个客户端对应一个唯一的UniqueOperationContext
ServiceContext::UniqueOperationContext Client::makeOperationContext() {
	//获取一个UniqueOperationContext类
    return getServiceContext()->makeOperationContext(this); //ServiceContext::makeOperationContext
}

//makeOperationContext中调用
void Client::setOperationContext(OperationContext* opCtx) {
    // We can only set the OperationContext once before resetting it.
    invariant(opCtx != NULL && _opCtx == NULL);
    _opCtx = opCtx;
}

void Client::resetOperationContext() {
    invariant(_opCtx != NULL);
    _opCtx = NULL;
}

std::string Client::clientAddress(bool includePort) const {
    if (!hasRemote()) {
        return "";
    }
    if (includePort) {
        return getRemote().toString();
    }
    return getRemote().host();
}

Client* Client::getCurrent() {
    return currentClient.get();
}

Client& cc() {
    invariant(haveClient());
    return *Client::getCurrent();
}

bool haveClient() {
    return static_cast<bool>(currentClient);
}

ServiceContext::UniqueClient Client::releaseCurrent() {
    invariant(haveClient());
    return std::move(currentClient);
}

void Client::setCurrent(ServiceContext::UniqueClient client) {
    invariant(!haveClient());
    currentClient = std::move(client);
}

}  // namespace mongo

