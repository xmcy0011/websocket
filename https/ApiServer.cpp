#include <glog/logging.h>

#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/algorithm/algorithm.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/proto/detail/ignore_unused.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/date_time.hpp>
#include <boost/thread.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <muduo/base/Exception.h>
#include <muduo/net/EventLoopThread.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TimerId.h>
#include <muduo/base/Logging.h>
#include <muduo/base/AsyncLogging.h>
#include <muduo/base/TimeZone.h>
#include <muduo/net/InetAddress.h>

#include <muduo/net/http/HttpContext.h>
#include <muduo/net/http/HttpRequest.h>
#include <muduo/net/http/HttpResponse.h>
#include <muduo/net/libwebsocket/ssl.h>

#include <sys/time.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <dirent.h>

#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string>
#include <iostream>
#include <math.h>
#include <deque>
#include <vector>
#include <list>
#include <map>
#include <algorithm>
#include <utility>
#include <functional>
#include <chrono>
#include <random>
#include <iomanip>
#include <sstream>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <ctype.h>
#include <malloc.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/timeb.h>
#include <fstream>
#include <ios>
#include "globalMacro.h"

extern int g_bisDebug;

// #ifdef __cplusplus
// extern "C" {
// #endif

#include "mymd5.h"
#include "base64.h"
#include "htmlcodec.h"
#include "aes.h"
#include "urlcodec.h"

// #ifdef __cplusplus
// }
// #endif

#include "ApiServer.h"

extern int g_EastOfUtc;

using namespace boost::gregorian;
using namespace boost::posix_time;
using namespace boost::local_time;

ApiServer::ApiServer(
	muduo::net::EventLoop* loop,
	const muduo::net::InetAddress& listenAddr,
	std::string const& cert_path, std::string const& private_key_path,
	std::string const& client_ca_cert_file_path,
	std::string const& client_ca_cert_dir_path)
		: server_(loop, listenAddr, "ApiServer")
        , threadTimer_(new muduo::net::EventLoopThread(muduo::net::EventLoopThread::ThreadInitCallback(), "TimerEventLoopThread"))
	, isdecrypt_(false)
	, whiteListControl_(eWhiteListCtrl::Close)
	, kTimeoutSeconds_(3)
	, kMaxConnections_(15000)
#ifdef _STAT_ORDER_QPS_
	, deltaTime_(10)
#endif
	, server_state_(ServiceRunning)
	, ttlUserLockSeconds_(1000)
	, ttlAgentLockSeconds_(500)
{
	
	server_.setConnectionCallback(
		std::bind(&ApiServer::onConnection, this, std::placeholders::_1));
	server_.setMessageCallback(
		std::bind(&ApiServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
		
	//添加OpenSSL支持 ///
	muduo::net::ssl::SSL_CTX_Init(
		cert_path,
		private_key_path,
		client_ca_cert_file_path, client_ca_cert_dir_path);
	
	//指定SSL_CTX
	server_.set_SSL_CTX(muduo::net::ssl::SSL_CTX_Get());
    
	threadTimer_->startLoop();
}

ApiServer::~ApiServer()
{
	//释放SSL_CTX
	muduo::net::ssl::SSL_CTX_free();
}

//启动HTTP业务线程 ///
//启动HTTP监听 ///
void ApiServer::start(int numThreads, int workerNumThreads, int maxSize) {
	
	//定时器检查
	threadTimer_->getLoop()->runAfter(3, std::bind(&OrderServer::refreshAgentInfo, this));
	threadTimer_->getLoop()->runAfter(3, std::bind(&OrderServer::refreshWhiteList, this));
	
	//一个网络IO线程对应一个业务线程，处理改网络IO上所有连接的HTTP请求，不涉及到共享数据可以做到无锁操作 ///
	for (int i = 0; i < workerNumThreads; ++i) {
		std::shared_ptr<muduo::ThreadPool> threadPool = std::make_shared<muduo::ThreadPool>("ThreadPool:" + std::to_string(i));
		threadPool->setThreadInitCallback(std::bind(&ApiServer::threadInit, this));
		//任务队列大小 ///
		threadPool->setMaxQueueSize(maxSize);
		threadPool->start(1);
		threadPool_.push_back(threadPool);
	}
	//worker线程数
	workerNumThreads_ = workerNumThreads;

	//网络IO线程数量
	server_.setThreadNum(numThreads);
	LOG_INFO << __FUNCTION__ << " --- *** "
		<< "\nHttpServer = " << server_.ipPort()
		<< " 网络IO线程数 = " << numThreads
		<< " worker 线程数 = " << workerNumThreads;
	
	//I/O线程数
	numThreads_ = numThreads;
	
	//开启了IP访问白名单功能 ///
	//Accept时候判断，socket底层控制，否则开启异步检查 ///
	if (whiteListControl_ == eWhiteListCtrl::OpenAccept) {
		//开启IP访问白名单检查
		server_.setConditionCallback(std::bind(&ApiServer::onConnection, this, std::placeholders::_1));
	}
	
	//启动HTTP监听
	server_.start();

	//必须等server_所有的IO线程都启动起来 ///
	//sleep(2);

	std::shared_ptr<muduo::net::EventLoopThreadPool> threadPool = server_.threadPool();
	std::vector<muduo::net::EventLoop*> loops = threadPool->getAllLoops();
	
	//初始化HashConnectionBucket池 ///
	for (size_t index = 0; index < loops.size(); ++index) {
		//为每个HashConnectionBucket对象创建定时器 ///
		bucketsPool_.push_back(HashConnectionBucket(*loops[index], index, kTimeoutSeconds_));
		//保存index到EventLoop上下文 ///
		loops[index]->setContext(EventLoopContext(index));
	}
	
	//最好 workerNumThreads = 2 * numThreads ///
	int next = 0;
	for (int i = 0; i < threadPool_.size(); ++i) {
		EventLoopContext* context = boost::any_cast<EventLoopContext>(loops[next]->getMutableContext());
		assert(context);
		context->addWorkerIndex(i);
		if (++next >= loops.size()) {
			next = 0;
		}
	}
	
	for (size_t index = 0; index < loops.size(); ++index) {
		//启动超时检查定时器 ///
		loops[index]->runAfter(1.0f, std::bind(&HashConnectionBucket::onTimer, &bucketsPool_[index]));
	}
}

//白名单检查 ///
bool ApiServer::onConnection(const InetAddress& peerAddr) {
	//Accept时候判断，socket底层控制，否则开启异步检查 ///
	//开启了IP访问白名单功能 ///
	assert(whiteListControl_ == eWhiteListCtrl::OpenAccept);
	//安全断言 ///
	server_.getLoop()->assertInLoopThread();
	{
		//管理员挂维护/恢复服务 ///
		std::map<in_addr_t, eApiVisit>::const_iterator it = admin_list_.find(peerAddr.ipNetEndian());
		if (it != admin_list_.end()) {
			return true;
		}
	}
	{
		//192.168.2.21:3640 192.168.2.21:3667
		std::map<in_addr_t, eApiVisit>::const_iterator it = white_list_.find(peerAddr.ipNetEndian());
		//在IP访问白名单中 ///
		return (it != white_list_.end()) && (eApiVisit::Enable == it->second);
	}
#if 0
	//节点维护中 ///
	if (server_state_ == ServiceRepairing) {
		return false;
	}
#endif
	return true;
}

//Connected/closed事件 ///
void ApiServer::onConnection(const muduo::net::TcpConnectionPtr& conn)
{
	//MY_TRY()
	if (conn->connected()) {
		{
			int32_t num = numConnected_.incrementAndGet();
			LOG_INFO << __FUNCTION__ << " --- *** " << "WEB端[" << conn->peerAddress().toIpPort() << "] -> HTTP服["
				<< conn->localAddress().toIpPort() << "] "
				<< (conn->connected() ? "UP" : "DOWN") << " " << num;
			
			//累计接收请求数 ///
			numTotalReq_.incrementAndGet();

			//最大连接数限制 ///
			if (num > kMaxConnections_) {
#if 0
				//不再发送数据
				conn->shutdown();
#elif 0
				//直接强制关闭连接
				conn->forceClose();
#else
				//HTTP应答包(header/body)
				muduo::net::HttpResponse rsp(false);
				setFailedResponse(rsp,
					muduo::net::HttpResponse::k404NotFound,
					"HTTP/1.1 600 访问量限制(" + std::to_string(kMaxConnections_) + ")\r\n\r\n");
				muduo::net::Buffer buf;
				rsp.appendToBuffer(&buf);
				conn->send(&buf);

				//延迟0.2s强制关闭连接
				conn->forceCloseWithDelay(0.2f);
#endif
				//会调用onMessage函数 ///
				assert(conn->getContext().empty());
				
				//累计未处理请求数 ///
				numTotalBadReq_.incrementAndGet();
				return;
			}
			//最好不要在网络IO线程中做逻辑处理 ///
#if 0
			//Accept时候判断，socket底层控制，否则开启异步检查 ///
			//开启了IP访问白名单功能 ///
			if (whiteListControl_ == eWhiteListCtrl::Open) {
				{
					bool is_ip_allowed = false;
					{
						READ_LOCK(white_list_mutex_);
						std::map<in_addr_t, eApiVisit>::const_iterator it = white_list_.find(conn->peerAddress().ipNetEndian());
						//在IP访问白名单中 ///
						is_ip_allowed = ((it != white_list_.end()) && (eApiVisit::Enable == it->second));
					}
					//不在IP访问白名单中 ///
					if (!is_ip_allowed) {
						//强制关闭 ///
#if 0
						conn->shutdown();
#elif 1
						conn->forceClose();
#else
						conn->forceCloseWithDelay(0.2f);
#endif
						return;
					}
				}
			}
#endif
			//shared_ptr局部对象，出了作用域，引用计数减1 ///
			//引用计数为0时会销毁Entry对象，触发析构函数调用强制关闭连接 ///
			EntryPtr entry(new Entry(muduo::net::WeakTcpConnectionPtr(conn), muduo::net::HttpContext()));
			{
				//为conn指定一个逻辑处理线程 ///
				EventLoopContext* context = boost::any_cast<EventLoopContext>(conn->getLoop()->getMutableContext());
				assert(context);
				int index = context->allocWorkerIndex();
				assert(index >= 0 && index < threadPool_.size());

				//为entry指定上下文信息 ///
				entry->setWorkerIndex(index);
			}
			{
				//对于HTTP请求来说，每一个conn都应该是独立的，指定一个独立线程处理即可，避免锁开销与多线程竞争抢占共享资源带来的性能损耗 ///
				EventLoopContext* context = boost::any_cast<EventLoopContext>(conn->getLoop()->getMutableContext());
				assert(context);
				int index = context->getBucketIndex();
				assert(index >= 0 && index < bucketsPool_.size());
				
				//连接成功，压入桶元素 ///
				//必须使用shared_ptr，持有entry引用计数(加1) ///
				//否则entry出了作用域，引用计数减1变为0销毁Entry对象，触发析构函数调用强制关闭连接 ///
				conn->getLoop()->runInLoop(std::bind(&HashConnectionBucket::pushBucket, &bucketsPool_[index], entry));
			}
			{
				//指定conn上下文信息，必须使用WeakEntryPtr弱指针，否则弹出Bucket无法析构 ///
				conn->setContext(WeakEntryPtr(entry));
			}
		}
	}
	else {
		int32_t num = numConnected_.decrementAndGet();
		LOG_INFO << __FUNCTION__ << " --- *** " << "WEB端[" << conn->peerAddress().toIpPort() << "] -> HTTP服["
			<< conn->localAddress().toIpPort() << "] "
			<< (conn->connected() ? "UP" : "DOWN") << " " << num;
	}
	//MY_CATCH()
}

//接收HTTP网络消息回调 ///
void ApiServer::onMessage(const muduo::net::TcpConnectionPtr& conn,
	muduo::net::Buffer* buf,
	muduo::Timestamp receiveTime)
{
	//MY_TRY()
	//超过最大连接数限制，被强制关闭连接 ///
	if (!conn || conn->getContext().empty()) {
		//LOG_ERROR << __FUNCTION__ << " --- *** " << "TcpConnectionPtr.conn max";
		return;
	}
	
	LOG_ERROR << __FUNCTION__ << " --- *** ";
	printf("----------------------------------------------\n");
	printf("%.*s\n", buf->readableBytes(), buf->peek());

	//先确定是HTTP数据报文，再解析 ///
	//assert(buf->readableBytes() > 4 && buf->findCRLFCRLF());

	WeakEntryPtr weakEntry(boost::any_cast<WeakEntryPtr>(conn->getContext()));
	EntryPtr entry(weakEntry.lock());
	if (likely(entry)) {
		//获取HttpContext对象
		muduo::net::HttpContext* context = boost::any_cast<muduo::net::HttpContext>(entry->getMutableContext());
		assert(context);
		//解析HTTP数据包
		if (!context->parseRequest(buf, receiveTime)) {
			//发生错误
		}
		else if (context->gotAll()) {
			//Accept时候判断，socket底层控制，否则开启异步检查 ///
			//开启了IP访问白名单功能 ///
			if (whiteListControl_ == eWhiteListCtrl::Open) {
				std::string ipaddr;
				{
					std::string ipaddrs = context->request().getHeader("X-Forwarded-For");
					if (ipaddrs.empty()) {
						ipaddr = conn->peerAddress().toIp();
					}
					else {
#if 0
						//第一个IP为客户端真实IP，可伪装，第二个IP为一级代理IP，第三个IP为二级代理IP
						std::string::size_type spos = ipaddrs.find_first_of(',');
						if (spos == std::string::npos) {
						}
						else {
							ipaddr = ipaddrs.substr(0, spos);
						}
#else
						boost::replace_all<std::string>(ipaddrs, " ", "");
						std::vector<std::string> vec;
						boost::algorithm::split(vec, ipaddrs, boost::is_any_of(","));
						for (std::vector<std::string>::const_iterator it = vec.begin();
							it != vec.end(); ++it) {
							if (!it->empty() &&
								boost::regex_match(*it, boost::regex(
										"^(1\\d{2}|2[0-4]\\d|25[0-5]|[1-9]\\d|[1-9])\\." \
										"(1\\d{2}|2[0-4]\\d|25[0-5]|[1-9]\\d|\\d)\\." \
										"(1\\d{2}|2[0-4]\\d|25[0-5]|[1-9]\\d|\\d)\\." \
										"(1\\d{2}|2[0-4]\\d|25[0-5]|[1-9]\\d|\\d)$"))) {
								
								if (strncasecmp(it->c_str(), "10.", 3) != 0 &&
									strncasecmp(it->c_str(), "192.168", 7) != 0 &&
									strncasecmp(it->c_str(), "172.16.", 7) != 0) {
									ipaddr = *it;
									break;
								}
							}
						}
#endif
					}
				}
				muduo::net::InetAddress peerAddr(muduo::StringArg(ipaddr), 0, false);
				bool is_ip_allowed = false;
				{
					//管理员挂维护/恢复服务 ///
					std::map<in_addr_t, eApiVisit>::const_iterator it = admin_list_.find(peerAddr.ipNetEndian());
					is_ip_allowed = (it != admin_list_.end());
				}
				if(!is_ip_allowed) {
					READ_LOCK(white_list_mutex_);
					std::map<in_addr_t, eApiVisit>::const_iterator it = white_list_.find(peerAddr.ipNetEndian());
					//在IP访问白名单中 ///
					is_ip_allowed = ((it != white_list_.end()) && (eApiVisit::Enable == it->second));
				}
				//不在IP访问白名单中 ///
				if (!is_ip_allowed) {
					//强制关闭 ///
#if 0
					//不再发送数据
					conn->shutdown();
#elif 1
					//直接强制关闭连接
					conn->forceClose();
#else
					//HTTP应答包(header/body)
					muduo::net::HttpResponse rsp(false);
					setFailedResponse(rsp,
						muduo::net::HttpResponse::k404NotFound,
						"HTTP/1.1 500 IP访问限制\r\n\r\n");
					muduo::net::Buffer buf;
					rsp.appendToBuffer(&buf);
					conn->send(&buf);

					//延迟0.2s强制关闭连接
					conn->forceCloseWithDelay(0.2f);
#endif
					//累计未处理请求数 ///
					numTotalBadReq_.incrementAndGet();
					return;
				}
			}

			//对于HTTP请求来说，每一个conn都应该是独立的，指定一个独立线程处理即可，避免锁开销与多线程竞争抢占共享资源带来的性能损耗 ///
			{
				EventLoopContext* context = boost::any_cast<EventLoopContext>(conn->getLoop()->getMutableContext());
				assert(context);
				int index = context->getBucketIndex();

				assert(index >= 0 && index < bucketsPool_.size());
				
				//收到消息包，更新桶元素 ///
				//必须使用shared_ptr，持有entry引用计数(加1) ///
				//否则entry出了作用域，引用计数减1变为0销毁Entry对象，触发析构函数调用强制关闭连接 ///
				conn->getLoop()->runInLoop(std::bind(&HashConnectionBucket::updateBucket, &bucketsPool_[index], entry));
			}

			//扔给任务消息队列处理 ///
			{
				int index = entry->getWorkerIndex();
				assert(index >= 0 && index < threadPool_.size());
				
				//无锁化处理，性能更好 ///
				//使用weak_ptr弱指针，不增加引用计数 ///
				//必须使用WeakEntryPtr弱指针，否则弹出Bucket无法析构 ///
				threadPool_[index]->run(std::bind(&ApiServer::asyncHttpHandler, this, weakEntry/*, context->request()*/, receiveTime));
			}
#endif
			return;
		}
		//发生错误
		//HTTP应答包(header/body)
		muduo::net::HttpResponse rsp(false);
		setFailedResponse(rsp,
			muduo::net::HttpResponse::k404NotFound,
			"HTTP/1.1 400 Bad Request\r\n\r\n");
		muduo::net::Buffer buf;
		rsp.appendToBuffer(&buf);
		conn->send(&buf);
		//释放HttpContext资源 ///
		context->reset();
#if 0
		//不再发送数据
		conn->shutdown();
#elif 0
		//直接强制关闭连接
		conn->forceClose();
#else
		//延迟0.2s强制关闭连接
		conn->forceCloseWithDelay(0.2f);
#endif
		//累计未处理请求数 ///
		numTotalBadReq_.incrementAndGet();
	}
	else {
		//累计未处理请求数 ///
		numTotalBadReq_.incrementAndGet();
		//Maybe called ///
		//LOG_ERROR << __FUNCTION__ << " --- *** " << "entry invalid";
	}
	//MY_CATCH()
}

//异步回调 ///
void ApiServer::asyncHttpHandler(const WeakEntryPtr& weakEntry/*, const muduo::net::HttpRequest& req*/, muduo::Timestamp receiveTime)
{
	//MY_TRY()
	//LOG_ERROR << __FUNCTION__;
	//刚开始还在想，会不会出现超时conn被异步关闭释放掉，而业务逻辑又被处理了，却发送不了的尴尬情况，
	//假如因为超时entry弹出bucket，引用计数减1，处理业务之前这里使用shared_ptr，持有entry引用计数(加1)，
	//如果持有失败，说明弹出bucket计数减为0，entry被析构释放，conn被关闭掉了，也就不会执行业务逻辑处理，
	//如果持有成功，即使超时entry弹出bucket，引用计数减1，但并没有减为0，entry也就不会被析构释放，conn也不会被关闭，
	//直到业务逻辑处理完并发送，entry引用计数减1变为0，析构被调用关闭conn(如果conn还存在的话，业务处理完也会主动关闭conn) ///
	EntryPtr entry(weakEntry.lock());
	if (likely(entry)) {
		muduo::net::TcpConnectionPtr conn(entry->getWeakConnPtr().lock());
		if (conn) {
#if 0
			//Accept时候判断，socket底层控制，否则开启异步检查 ///
			//开启了IP访问白名单功能 ///
			if (whiteListControl_ == eWhiteListCtrl::Open) {

				bool is_ip_allowed = false;
				{
					READ_LOCK(white_list_mutex_);
					std::map<in_addr_t, eApiVisit>::const_iterator it = white_list_.find(conn->peerAddress().ipNetEndian());
					//在IP访问白名单中 ///
					is_ip_allowed = ((it != white_list_.end()) && (eApiVisit::Enable == it->second));
				}
				//不在IP访问白名单中 ///
				if (!is_ip_allowed) {
					//强制关闭 ///
#if 0
					//不再发送数据
					conn->shutdown();
#elif 0
					//直接强制关闭连接
					conn->forceClose();
#else
					//HTTP应答包(header/body)
					muduo::net::HttpResponse rsp(false);
					setFailedResponse(rsp,
						muduo::net::HttpResponse::k404NotFound,
						"HTTP/1.1 500 IP访问限制\r\n\r\n");
					muduo::net::Buffer buf;
					rsp.appendToBuffer(&buf);
					conn->send(&buf);

					//延迟0.2s强制关闭连接
					conn->forceCloseWithDelay(0.2f);
#endif
					return;
				}
			}
#endif
			//获取HttpContext对象
			muduo::net::HttpContext* context = boost::any_cast<muduo::net::HttpContext>(entry->getMutableContext());
			assert(context);
			assert(context->gotAll());

			const string& connection = context->request().getHeader("Connection");
			//是否保持HTTP长连接
			bool close = (connection == "close") ||
				(context->request().getVersion() == muduo::net::HttpRequest::kHttp10 && connection != "Keep-Alive");
			//HTTP应答包(header/body)
			muduo::net::HttpResponse rsp(close);
			//请求处理回调，但非线程安全的 processHttpRequest
			{
				//assert(httpCallback_);
				//httpCallback_(context->request(), rsp);
				processHttpRequest(context->request(), rsp, conn->peerAddress(), receiveTime);
			}
			//应答消息
			{
				muduo::net::Buffer buf;
				rsp.appendToBuffer(&buf);
				conn->send(&buf);
			}
			//非HTTP长连接则关闭
			if (rsp.closeConnection()) {
#if 0
				//不再发送数据
				conn->shutdown();
#elif 0
				//直接强制关闭连接
				conn->forceClose();
#else
				//延迟0.2s强制关闭连接
				conn->forceCloseWithDelay(0.2f);
#endif
			}
			//释放HttpContext资源 ///
			context->reset();
		}
		else {
			//累计未处理请求数 ///
			numTotalBadReq_.incrementAndGet();
			//Maybe called ///
			//LOG_ERROR << __FUNCTION__ << " --- *** " << "TcpConnectionPtr.conn invalid";
		}
	}
	else {
		//累计未处理请求数 ///
		numTotalBadReq_.incrementAndGet();
		//Maybe called ///
		//LOG_ERROR << __FUNCTION__ << " --- *** " << "entry invalid";
	}
	//MY_CATCH()
}

//解析请求 ///
//strQuery string req.query()
bool ApiServer::parseQuery(std::string const& queryStr, HttpParams& params, std::string& errmsg)
{
	params.clear();
	/*LOG_WARN*/LOG_DEBUG << "--- *** " << "\n" << queryStr;
	do {
		std::string subStr;
		std::string::size_type npos = queryStr.find_first_of('?');
		if (npos != std::string::npos) {
			//skip '?' ///
			subStr = queryStr.substr(npos + 1, std::string::npos);
		}
		else {
			subStr = queryStr;
		}
		if (subStr.empty()) {
			break;
		}
		for (;;) {
			//key value separate ///
			std::string::size_type kpos = subStr.find_first_of('=');
			if (kpos == std::string::npos) {
				break;
			}
			//next start ///
			std::string::size_type spos = subStr.find_first_of('&');
			if (spos == std::string::npos) {
				std::string key = subStr.substr(0, kpos);
				//skip '=' ///
				std::string val = subStr.substr(kpos + 1, std::string::npos);
				params[key] = val;
				break;
			}
			else if (kpos < spos) {
				std::string key = subStr.substr(0, kpos);
				//skip '=' ///
				std::string val = subStr.substr(kpos + 1, spos - kpos - 1);
				params[key] = val;
				//skip '&' ///
				subStr = subStr.substr(spos + 1, std::string::npos);
			}
			else {
				break;
			}
		}
	} while (0);
	std::string keyValues;
	for (auto param : params) {
		keyValues += "\n--- **** " + param.first + "=" + param.second;
	}
	//LOG_DEBUG << "--- *** " << keyValues;
	return true;
}

//请求字符串 ///
std::string ApiServer::getRequestStr(muduo::net::HttpRequest const& req) {
	std::string headers;
	for (std::map<string, string>::const_iterator it = req.headers().begin();
		it != req.headers().end(); ++it) {
		headers += it->first + ": " + it->second + "\n";
	}
	std::stringstream ss;
	ss << "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
		<< "<xs:root xmlns:xs=\"http://www.w3.org/2001/XMLSchema\">"
		<< "<xs:head>" << headers << "</xs:head>"
		<< "<xs:body>"
		<< "<xs:method>" << req.methodString() << "</xs:method>"
		<< "<xs:path>" << req.path() << "</xs:path>"
		<< "<xs:query>" << HTML::Encode(req.query()) << "</xs:query>"
		<< "</xs:body>"
		<< "</xs:root>";
	return ss.str();
}

//按照占位符来替换 ///
static void replace(std::string& json, const std::string& placeholder, const std::string& value) {
	boost::replace_all<std::string>(json, "\"" + placeholder + "\"", value);
}

/* 返回格式 ///
	{
		"maintype": "/GameHandle",
			"type": 2,
			"data":
			{
				"orderid":"",
				"agentid": 10000,
				"account": "999",
				"score": 10000,
				"code": 0,
				"errmsg":"",
			}
	}
*/
// 构造返回结果 ///
std::string ApiServer::createResponse(
	int32_t opType,
	std::string const& orderId,
	uint32_t agentId,
	std::string account, int64_t score,
	int errcode, std::string const& errmsg, bool debug)
{
#if 0
	Json::Value root, data;
	if (debug) data["orderid"] = orderId;
	if (debug) data["agentid"] = agentId;
	data["account"] = account;
	data["score"] = (uint32_t)score;
	data["code"] = (int32_t)errcode;
	if (debug) data["errmsg"] = errmsg;
	// 外层json
	root["maintype"] = "/GameHandle";
	root["type"] = opType;
	root["data"] = data;
	Json::FastWriter writer;
	std::string json = writer.write(root);
	boost::replace_all<std::string>(json, "\\", "");
	return json;
#else
	boost::property_tree::ptree root, data;
	if (debug) data.put("orderid", orderId);
	if (debug) data.put("agentid", ":agentid");
	data.put("account", account);
	data.put("score", ":score");
	data.put("code", ":code");
	if (debug) data.put("errmsg", errmsg);
	// 外层json
	root.put("maintype", "/GameHandle");
	root.put("type", ":type");
	root.add_child("data", data);
	std::stringstream s;
	boost::property_tree::json_parser::write_json(s, root, false);
	std::string json = s.str();
	if (debug) replace(json, ":agentid", std::to_string(agentId));
	replace(json, ":score", std::to_string(score));
	replace(json, ":code", std::to_string(errcode));
	replace(json, ":type", std::to_string(opType));
	boost::replace_all<std::string>(json, "\\", "");
	return json;
#endif
}

//最近一次请求(上分或下分操作的elapsed detail)
void ApiServer::createLatestElapsed(
	boost::property_tree::ptree& latest,
	std::string const& op, std::string const& key, double elapsed) {
	//{"op":"mongo.collect", "dt":1000, "key":}
	//{"op":"mongo.insert", "dt":1000, "key":}
	//{"op":"mongo.update", "dt":1000, "key":}
	//{"op":"mongo.query", "dt":1000, "key":}
	//{"op":"redis.insert", "dt":1000, "key":}
	//{"op":"redis.update", "dt":1000, "key":}
	//{"op":"redis.query", "dt":1000, "key":}
#if 0
	boost::property_tree::ptree data, item;
	data.put("op", op);
	data.put("key", key);
	data.put("dt", ":dt");
	std::stringstream s;
	boost::property_tree::json_parser::write_json(s, data, false);
	std::string json = s.str();
	replace(json, ":dt", std::to_string(elapsed));
	std::stringstream ss(json);
	boost::property_tree::json_parser::read_json(ss, item);
	latest.push_back(std::make_pair("", item));
#else
	boost::property_tree::ptree item;
	item.put("op", op);
	item.put("key", key);
	item.put("dt", elapsed);
	latest.push_back(std::make_pair("", item));
#endif
}

//监控数据
std::string ApiServer::createMonitorData(
	boost::property_tree::ptree const& latest, double totalTime, int timeout,
	int64_t requestNum, int64_t requestNumSucc, int64_t requestNumFailed, double ratio,
	int64_t requestNumTotal, int64_t requestNumTotalSucc, int64_t requestNumTotalFailed, double ratioTotal, int testTPS) {
	boost::property_tree::ptree root, stat, history;
	//最近一次请求 latest
	root.add_child("latest", latest);
	//统计间隔时间 totalTime
	root.put("stat_dt", ":stat_dt");
	//估算每秒请求处理数 testTPS
	root.put("test_TPS", ":test_TPS");
	//请求超时时间 kTimeoutSeconds_
	root.put("req_timeout", ":req_timeout");
	{
		//统计请求次数 requestNum
		stat.put("stat_total", ":stat_total");
		//统计成功次数 requestNumSucc
		stat.put("stat_succ", ":stat_succ");
		//统计失败次数 requestNumFailed
		stat.put("stat_fail", ":stat_fail");
		//统计命中率 ratio
		stat.put("stat_ratio", ":stat_ratio");
		root.add_child("stat", stat);
	}
	{
		//历史请求次数 requestNumTotal
		history.put("total", ":total");
		//历史成功次数 requestNumTotalSucc
		history.put("succ", ":succ");
		//历史失败次数 requestNumTotalFailed
		history.put("fail", ":fail");
		//历史命中率 ratioTotal
		history.put("ratio", ":ratio");
		root.add_child("history", history);
	}
	std::stringstream s;
	boost::property_tree::json_parser::write_json(s, root, false);
	std::string json = s.str();
	replace(json, ":stat_dt", std::to_string(totalTime));
	replace(json, ":test_TPS", std::to_string(testTPS));
	replace(json, ":req_timeout", std::to_string(timeout));
	replace(json, ":stat_total", std::to_string(requestNum));
	replace(json, ":stat_succ", std::to_string(requestNumSucc));
	replace(json, ":stat_fail", std::to_string(requestNumFailed));
	replace(json, ":stat_ratio", std::to_string(ratio));
	replace(json, ":total", std::to_string(requestNumTotal));
	replace(json, ":succ", std::to_string(requestNumTotalSucc));
	replace(json, ":fail", std::to_string(requestNumTotalFailed));
	replace(json, ":ratio", std::to_string(ratioTotal));
	return json;
}

//处理HTTP回调 ///
void ApiServer::processHttpRequest(const muduo::net::HttpRequest& req, muduo::net::HttpResponse& rsp, muduo::net::InetAddress const& peerAddr, muduo::Timestamp receiveTime)
{
	//LOG_INFO << __FUNCTION__ << " --- *** " << getRequestStr(req);
	rsp.setStatusCode(muduo::net::HttpResponse::k200Ok);
	rsp.setStatusMessage("OK");
	//注意要指定connection状态
	rsp.setCloseConnection(true);
	rsp.addHeader("Server", "MUDUO");

	if (req.path() == "/") {
#if 0
		rsp.setContentType("text/html;charset=utf-8");
		std::string now = muduo::Timestamp::now().toFormattedString();
		rsp.setBody("<html><body>Now is " + now + "</body></html>");
#else
		//HTTP应答包(header/body)
		setFailedResponse(rsp,
			muduo::net::HttpResponse::k404NotFound,
			"HTTP/1.1 404 Not Found\r\n\r\n");
#endif
	}
	else if (req.path() == "/GameHandle") {
		std::string rspdata;
		boost::property_tree::ptree latest;
		int errcode = eApiErrorCode::NoError;
		std::string errmsg;
		int testTPS = 0;
#ifdef _STAT_ORDER_QPS_
		
		//起始时间戳(微秒) ///
		static muduo::Timestamp orderTimeStart_;
		//统计请求次数 ///
		static muduo::AtomicInt32 numOrderRequest_;
		//历史请求次数 ///
		static muduo::AtomicInt32 numOrderRequestTotal_;
		//统计成功次数 ///
		static muduo::AtomicInt32 numOrderRequestSucc_;
		//统计失败次数 ///
		static muduo::AtomicInt32 numOrderRequestFailed_;
		//历史成功次数 ///
		static muduo::AtomicInt32 numOrderRequestTotalSucc_;
		//历史失败次数 ///
		static muduo::AtomicInt32 numOrderRequestTotalFailed_;
		//原子操作判断 ///
		{
			static volatile long value = 0;
			if (0 == __sync_val_compare_and_swap(&value, 0, 1)) {
				//起始时间戳(微秒)
				orderTimeStart_ = muduo::Timestamp::now();
			}
		}
		//本次请求开始时间戳(微秒)
		muduo::Timestamp timestart = muduo::Timestamp::now();
#endif
		//节点维护中不提供上下分服务 ///
		if (server_state_ == ServiceRepairing) {
			//HTTP应答包(header/body)
			setFailedResponse(rsp,
				muduo::net::HttpResponse::k404NotFound,
				"HTTP/1.1 405 服务维护中\r\n\r\n");
		}
		//请求处理逻辑 ///
		else {
			/*std::string */rspdata = OrderProcess(req.query(), receiveTime, errcode, errmsg, latest, testTPS);
			//LOG_INFO << "--- *** " << "\n" << rspdata;
#ifdef _STAT_ORDER_QPS_
			if (errcode == eApiErrorCode::NoError) {
				//统计成功次数 ///
				numOrderRequestSucc_.incrementAndGet();
				//历史成功次数 ///
				numOrderRequestTotalSucc_.incrementAndGet();
			}
			else {
				//统计失败次数 ///
				numOrderRequestFailed_.incrementAndGet();
				//历史失败次数 ///
				numOrderRequestTotalFailed_.incrementAndGet();
			}
#endif
			rsp.setContentType("application/json;charset=utf-8");
			rsp.setBody(rspdata);
		}
#ifdef _STAT_ORDER_QPS_
		//本次请求结束时间戳(微秒)
		muduo::Timestamp timenow = muduo::Timestamp::now();
		
		//统计请求次数 ///
		numOrderRequest_.incrementAndGet();
		//历史请求次数 ///
		numOrderRequestTotal_.incrementAndGet();

		//原子操作判断 ///
		static volatile long value = 0;
		if (0 == __sync_val_compare_and_swap(&value, 0, 1)) {
			//间隔时间(s)打印一次
			//static int deltaTime_ = 10;
			//统计间隔时间(s)
			double totalTime = muduo::timeDifference(timenow, orderTimeStart_);
			//if (totalTime >= (double)deltaTime_) {
				//最近一次请求耗时(s)
				double timdiff = muduo::timeDifference(timenow, timestart);
				char pid[128] = { 0 };
				snprintf(pid, sizeof(pid), "PID[%07d]", getpid());
				//统计请求次数 ///
				int64_t	requestNum = numOrderRequest_.get();
				//统计成功次数 ///
				int64_t requestNumSucc = numOrderRequestSucc_.get();
				//统计失败次数 ///
				int64_t requestNumFailed = numOrderRequestFailed_.get();
				//统计命中率 ///
				double ratio = (double)(requestNumSucc) / (double)(requestNum);
				//历史请求次数 ///
				int64_t	requestNumTotal = numOrderRequestTotal_.get();
				//历史成功次数 ///
				int64_t requestNumTotalSucc = numOrderRequestTotalSucc_.get();
				//历史失败次数 ///
				int64_t requestNumTotalFailed = numOrderRequestTotalFailed_.get();
				//历史命中率 ///
				double ratioTotal = (double)(requestNumTotalSucc) / (double)(requestNumTotal);
				//平均请求耗时(s) ///
				double avgTime = totalTime / requestNum;
				//每秒请求次数(QPS) ///
				int64_t avgNum = (int64_t)(requestNum / totalTime);
				std::stringstream s;
				boost::property_tree::json_parser::write_json(s, latest, true);
				std::string json = s.str();
#if 1
				LOG_ERROR << __FUNCTION__ << " --- *** "
					<< "\n--- *** ------------------------------------------------------\n"
					<< json
					<< "--- *** " << pid << "[注单]I/O线程数[" << numThreads_ << "] 业务线程数[" << workerNumThreads_ << "] 累计接收请求数[" << numTotalReq_.get() << "] 累计未处理请求数[" << numTotalBadReq_.get() << "]\n"
					<< "--- *** " << pid << "[注单]本次统计间隔时间[" << totalTime << "]s 请求超时时间[" << kTimeoutSeconds_ << "]s\n"
					<< "--- *** " << pid << "[注单]本次统计请求次数[" << requestNum << "] 成功[" << requestNumSucc << "] 失败[" << requestNumFailed << "] 命中率[" << ratio << "]\n"
					<< "--- *** " << pid << "[注单]最近一次请求耗时[" << timdiff * muduo::Timestamp::kMicroSecondsPerSecond / 1000 << "]ms [" << errmsg << "]\n"
					<< "--- *** " << pid << "[注单]平均请求耗时[" << avgTime * muduo::Timestamp::kMicroSecondsPerSecond / 1000 << "]ms\n"
					<< "--- *** " << pid << "[注单]每秒请求次数(QPS) = [" << avgNum << "] 单线程每秒请求处理数(TPS) = [" << testTPS << "] 预计每秒请求处理总数(TPS) = [" << testTPS * workerNumThreads_ << "]\n"
					<< "--- *** " << pid << "[注单]历史请求次数[" << requestNumTotal << "] 成功[" << requestNumTotalSucc << "] 失败[" << requestNumTotalFailed << "] 命中率[" << ratioTotal << "]\n\n";
#else
				std::stringstream ss; ss << json
					<< "--- *** " << pid << "[注单]I/O线程数[" << numThreads_ << "] 业务线程数[" << workerNumThreads_ << "] 累计接收请求数[" << numTotalReq_.get() << "] 累计未处理请求数[" << numTotalBadReq_.get() << "]\n"
					<< "--- *** " << pid << "[注单]本次统计间隔时间[" << totalTime << "]s 请求超时时间[" << kTimeoutSeconds_ << "]s\n"
					<< "--- *** " << pid << "[注单]本次统计请求次数[" << requestNum << "] 成功[" << requestNumSucc << "] 失败[" << requestNumFailed << "] 命中率[" << ratio << "]\n"
					<< "--- *** " << pid << "[注单]最近一次请求耗时[" << timdiff * muduo::Timestamp::kMicroSecondsPerSecond / 1000 << "]ms [" << errmsg << "]\n"
					<< "--- *** " << pid << "[注单]平均请求耗时[" << avgTime * muduo::Timestamp::kMicroSecondsPerSecond / 1000 << "]ms\n"
					<< "--- *** " << pid << "[注单]每秒请求次数(QPS) = [" << avgNum << "] 单线程每秒请求处理数(TPS) = [" << testTPS << "] 预计每秒请求处理总数(TPS) = [" << testTPS * workerNumThreads_ << "]\n"
					<< "--- *** " << pid << "[注单]历史请求次数[" << requestNumTotal << "] 成功[" << requestNumTotalSucc << "] 失败[" << requestNumTotalFailed << "] 命中率[" << ratioTotal << "]\n\n";
#endif
				if (totalTime >= (double)deltaTime_) {
					//更新redis监控字段
					std::string monitordata = createMonitorData(latest, totalTime, kTimeoutSeconds_,
						requestNum, requestNumSucc, requestNumFailed, ratio,
						requestNumTotal, requestNumTotalSucc, requestNumTotalFailed, ratioTotal, testTPS);
					REDISCLIENT.set("s.monitor.order", monitordata);
				}
				//重置起始时间戳(微秒) ///
				orderTimeStart_ = timenow;
				//重置统计请求次数 ///
				numOrderRequest_.getAndSet(0);
				//重置统计成功次数 ///
				numOrderRequestSucc_.getAndSet(0);
				//重置统计失败次数 ///
				numOrderRequestFailed_.getAndSet(0);
			//}
			__sync_val_compare_and_swap(&value, 1, 0);
		}
#endif
#if 0
		//rsp.setBody(rspdata);
#else
		rsp.setContentType("application/xml;charset=utf-8");
		rsp.setBody(getRequestStr(req));
#endif
	}
	//刷新代理信息agent_info ///
	else if (req.path() == "/refreshAgentInfo") {
		//管理员挂维护/恢复服务 ///
		std::map<in_addr_t, eApiVisit>::const_iterator it = admin_list_.find(peerAddr.ipNetEndian());
		if (it != admin_list_.end()) {
			rsp.setContentType("text/plain;charset=utf-8");
			if (!refreshAgentInfo()) {
				rsp.setBody("failed");
			}
			else {
				rsp.setBody("success");
			}
		}
		else {
			//HTTP应答包(header/body)
			setFailedResponse(rsp,
				muduo::net::HttpResponse::k404NotFound,
				"HTTP/1.1 504 权限不够\r\n\r\n");
		}
	}
	//刷新白名单信息white_list ///
	else if (req.path() == "/refreshWhiteList") {
		//管理员挂维护/恢复服务 ///
		std::map<in_addr_t, eApiVisit>::const_iterator it = admin_list_.find(peerAddr.ipNetEndian());
		if (it != admin_list_.end()) {
			rsp.setContentType("text/plain;charset=utf-8");
			refreshWhiteList();
			rsp.setBody("success");
		}
		else {
			//HTTP应答包(header/body)
			setFailedResponse(rsp,
				muduo::net::HttpResponse::k404NotFound,
				"HTTP/1.1 504 权限不够\r\n\r\n");
		}
	}
	//请求挂维护/恢复服务 status=0挂维护 status=1恢复服务 ///
	else if (req.path() == "/repairApiServer") {
		//管理员挂维护/恢复服务 ///
		std::map<in_addr_t, eApiVisit>::const_iterator it = admin_list_.find(peerAddr.ipNetEndian());
		if (it != admin_list_.end()) {
			rsp.setContentType("text/plain;charset=utf-8");
			if (!repairApiServer(req.query())) {
				rsp.setBody("failed");
			}
			else {
				rsp.setBody("success");
			}
		}
		else {
			//HTTP应答包(header/body)
			setFailedResponse(rsp,
				muduo::net::HttpResponse::k404NotFound,
				"HTTP/1.1 504 权限不够\r\n\r\n");
		}
	}
	else if (req.path() == "/help") {
		//管理员挂维护/恢复服务 ///
		std::map<in_addr_t, eApiVisit>::const_iterator it = admin_list_.find(peerAddr.ipNetEndian());
		if (it != admin_list_.end()) {
			rsp.setContentType("text/html;charset=utf-8");
			rsp.setBody("<html>"
				"<head><title>help</title></head>"
				"<body>"
				"<h4>/refreshAgentInfo</h4>"
				"<h4>/refreshWhiteList</h4>"
				"<h4>/repairApiServer?status=0|1(status=0挂维护 status=1恢复服务)</h4>"
				"</body>"
				"</html>");
		}
		else {
			//HTTP应答包(header/body)
			setFailedResponse(rsp,
				muduo::net::HttpResponse::k404NotFound,
				"HTTP/1.1 504 权限不够\r\n\r\n");
		}
	}
	else {
#if 1
		//HTTP应答包(header/body)
		setFailedResponse(rsp,
			muduo::net::HttpResponse::k404NotFound,
			"HTTP/1.1 404 Not Found\r\n\r\n");
#else
		rsp.setBody("<html><head><title>httpServer</title></head>"
			"<body><h1>Not Found</h1>"
			"</body></html>");
		//rsp.setStatusCode(muduo::net::HttpResponse::k404NotFound);
#endif
	}
}

//请求挂维护/恢复服务 status=0挂维护 status=1恢复服务 ///
void ApiServer::repairApiServerNotify(std::string const& msg) {
	int status;
	do {
		if (msg.empty() || (status = atol(msg.c_str())) < 0) {
			break;
		}
		//请求挂维护 ///
		if (status == ServiceRepairing) {
			/* 如果之前服务中, 尝试挂维护中, 并返回之前状态
			* 如果返回服务中, 说明刚好挂维护成功, 否则说明之前已被挂维护 */
			if (ServiceRunning == __sync_val_compare_and_swap(&server_state_, ServiceRunning, ServiceRepairing)) {
				
			}
		}
		//请求恢复服务 ///
		else if (status == ServiceRunning) {
			/* 如果之前挂维护中, 尝试恢复服务, 并返回之前状态
			* 如果返回挂维护中, 说明刚好恢复服务成功, 否则说明之前已恢复服务 */
			if (ServiceRepairing == __sync_val_compare_and_swap(&server_state_, ServiceRepairing, ServiceRunning)) {
				
			}
		}
	} while (0);
}

//请求挂维护/恢复服务 status=0挂维护 status=1恢复服务 ///
bool ApiServer::repairApiServer(std::string const& reqStr) {
	std::string errmsg;
	int status;
	do {
		//解析参数 ///
		HttpParams params;
		if (!parseQuery(reqStr, params, errmsg)) {
			break;
		}
		HttpParams::const_iterator statusKey = params.find("status");
		if (statusKey == params.end() || statusKey->second.empty() ||
			(status = atol(statusKey->second.c_str())) < 0) {
			break;
		}
		//请求挂维护 ///
		if (status == ServiceRepairing) {
			/* 如果之前服务中, 尝试挂维护中, 并返回之前状态
			* 如果返回服务中, 说明刚好挂维护成功, 否则说明之前已被挂维护 */
			if (ServiceRunning == __sync_val_compare_and_swap(&server_state_, ServiceRunning, ServiceRepairing)) {
				
			}
			return true;
		}
		//请求恢复服务 ///
		else if (status == ServiceRunning) {
			/* 如果之前挂维护中, 尝试恢复服务, 并返回之前状态
			* 如果返回挂维护中, 说明刚好恢复服务成功, 否则说明之前已恢复服务 */
			if (ServiceRepairing == __sync_val_compare_and_swap(&server_state_, ServiceRepairing, ServiceRunning)) {
				
			}
			return true;
		}
	} while (0);
	return false;
}

//刷新所有agent_info信息 ///
//1.web后台更新代理通知刷新
//2.游戏启动刷新一次
//3.redis广播通知刷新一次
bool ApiServer::refreshAgentInfo()
{
	//LOG_DEBUG << __FUNCTION__;
	{
		WRITE_LOCK(agent_info_mutex_);
		agent_info_.clear();
	}
	//std::string format;
	for (std::map<int32_t, agent_info_t>::const_iterator it = agent_info_.begin();
		it != agent_info_.end(); ++it) {
		//std::stringstream ss;
		LOG_DEBUG/*ss*/ << "--- *** " << "代理信息\n"
			<<"--- *** agentId[" << it->second.agentId
			<< "] score[" << it->second.score
			<< "] status[" << it->second.status
			<< "] md5code[" << it->second.md5code
			<< "] descode[" << it->second.descode
			<< "] cooperationtype[" << it->second.cooperationtype << "]";
		//format += ss.str().c_str();
	}
	//std::cout << format << std::endl;
	//LOG_DEBUG << "--- *** " << "代理信息" << format;
	return true;
}

//刷新所有IP访问白名单信息 ///
//1.web后台更新白名单通知刷新
//2.游戏启动刷新一次
//3.redis广播通知刷新一次 ///
void ApiServer::refreshWhiteList() {
	//开启了IP访问白名单功能 ///
	if (whiteListControl_ == eWhiteListCtrl::OpenAccept) {
		//Accept时候判断，socket底层控制，否则开启异步检查 ///
		server_.getLoop()->runInLoop(std::bind(&ApiServer::refreshWhiteListInLoop, this));
	}
	else if (whiteListControl_ == eWhiteListCtrl::Open) {
		//同步刷新IP访问白名单
		refreshWhiteListSync();
	}
}

//同步刷新IP访问白名单
bool ApiServer::refreshWhiteListSync() {
	//Accept时候判断，socket底层控制，否则开启异步检查 ///
	assert(whiteListControl_ == eWhiteListCtrl::Open);
	{
		WRITE_LOCK(white_list_mutex_);
		white_list_.clear();
	}
	//std::string format;
	for (std::map<in_addr_t, eApiVisit>::const_iterator it = white_list_.begin();
		it != white_list_.end(); ++it) {
		//std::stringstream ss;
		LOG_DEBUG/*ss*/ << "--- *** " << "IP访问白名单\n"
			<< "--- *** ipaddr[" << Inet2Ipstr(it->first) << "] status[" << it->second << "]";
		//format += ss.str().c_str();
	}
	//std::cout << format << std::endl;
	//LOG_DEBUG << "--- *** " << "IP访问白名单" << format;
	return true;
}

bool ApiServer::refreshWhiteListInLoop() {
	//Accept时候判断，socket底层控制，否则开启异步检查 ///
	assert(whiteListControl_ == eWhiteListCtrl::OpenAccept);
	//安全断言 ///
	server_.getLoop()->assertInLoopThread();
	white_list_.clear();
	//std::string format;
	for (std::map<in_addr_t, eApiVisit>::const_iterator it = white_list_.begin();
		it != white_list_.end(); ++it) {
		//std::stringstream ss;
		LOG_DEBUG/*ss*/ << "--- *** " << "IP访问白名单\n"
			<< "--- *** ipaddr[" << Inet2Ipstr(it->first) << "] status[" << it->second << "]";
		//format += ss.str().c_str();
	}
	//std::cout << format << std::endl;
	//LOG_DEBUG << "--- *** " << "IP访问白名单" << format;
	return true;
}

//订单处理函数 ///
std::string ApiServer::OrderProcess(std::string const& reqStr, muduo::Timestamp receiveTime, int& errcode, std::string& errmsg, boost::property_tree::ptree& latest, int& testTPS) {
	int opType = 0;
	int agentId = 0;
#ifdef _NO_LOGIC_PROCESS_
	int64_t userId = 0;
#endif
	std::string account;
	std::string orderId;
	int64_t score = 0;
	std::string timestamp;
	std::string paraValue, key;
	agent_info_t /*_agent_info = { 0 },*/* p_agent_info = NULL;
	//
	//int errcode = eApiErrorCode::NoError;
	//std::string errmsg;
	do {
		//解析参数 ///
		HttpParams params;
		if (!parseQuery(reqStr, params, errmsg)) {
			// 传输参数错误
			errcode = eApiErrorCode::GameHandleParamsError;
			errmsg = "bad request";
			break;
		}
		//明文请求，不解码 ///
		if (!isdecrypt_) {
			//type
			HttpParams::const_iterator typeKey = params.find("type");
			if (typeKey == params.end() || typeKey->second.empty() ||
				(opType = atol(typeKey->second.c_str())) < 0) {
				// 操作类型参数错误
				errcode = eApiErrorCode::GameHandleOperationTypeError;
				errmsg += "type ";
			}
			//2.上分 3.下分
			if (opType != int(eApiType::OpAddScore) && opType != int(eApiType::OpSubScore)) {
				// 操作类型参数错误
				errcode = eApiErrorCode::GameHandleOperationTypeError;
				errmsg += "type value ";
			}
			//agentid
			HttpParams::const_iterator agentIdKey = params.find("agentid");
			if (agentIdKey == params.end() || agentIdKey->second.empty() ||
				(agentId = atol(agentIdKey->second.c_str())) <= 0) {
				// 传输参数错误
				errcode = eApiErrorCode::GameHandleParamsError;
				errmsg += "agentid ";
				break;
			}
#ifdef _NO_LOGIC_PROCESS_
			//userid
			HttpParams::const_iterator userIdKey = params.find("userid");
			if (userIdKey == params.end() || userIdKey->second.empty() ||
				(userId = atoll(userIdKey->second.c_str())) <= 0) {
				errcode = eApiErrorCode::GameHandleParamsError;
				errmsg += "userid ";
			}
#endif
			//account
			HttpParams::const_iterator accountKey = params.find("account");
			if (accountKey == params.end() || accountKey->second.empty()) {
				// 传输参数错误
				errcode = eApiErrorCode::GameHandleParamsError;
				errmsg += "account ";
			}
			else {
				account = accountKey->second;
			}
			//orderid
			HttpParams::const_iterator orderIdKey = params.find("orderid");
			if (orderIdKey == params.end() || orderIdKey->second.empty()) {
				// 传输参数错误
				errcode = eApiErrorCode::GameHandleParamsError;
				errmsg += "orderid ";
			}
			else {
				orderId = orderIdKey->second;
			}
			//score
			HttpParams::const_iterator scoreKey = params.find("score");
			if (scoreKey == params.end() || scoreKey->second.empty() || !IsDigitStr(scoreKey->second) ||
				(score = atoll(scoreKey->second.c_str())) <= 0) {
				// 传输参数错误
				errcode = eApiErrorCode::GameHandleParamsError;
				errmsg += "score ";
			}
			//有错误发生 ///
			if (errcode != 0) {
				errmsg += "invalid";
				break;
			}
			// 获取当前代理数据
			//agent_info_t _agent_info = { 0 };
			{
				READ_LOCK(agent_info_mutex_);
				std::map<int32_t, agent_info_t>::/*const_*/iterator it = agent_info_.find(agentId);
				if (it == agent_info_.end()) {
					// 代理ID不存在或代理已停用
					errcode = eApiErrorCode::GameHandleProxyIDError;
					errmsg = "agent_info not found";
					break;
				}
				else {
					p_agent_info = &it->second;
				}
			}
			// 没有找到代理，判断代理的禁用状态(0正常 1停用)
			if (p_agent_info->status == 1) {
				// 代理ID不存在或代理已停用
				errcode = eApiErrorCode::GameHandleProxyIDError;
				errmsg = "agent.status error";
				break;
			}
#ifndef _NO_LOGIC_PROCESS_
			//上下分操作 ///
			errcode = doOrderExecute(opType, account, score, *p_agent_info, orderId, errmsg, latest, testTPS);
#endif
			break;
		}
		//type
		HttpParams::const_iterator typeKey = params.find("type");
		if (typeKey == params.end() || typeKey->second.empty() ||
			(opType = atol(typeKey->second.c_str())) < 0) {
			// 操作类型参数错误
			errcode = eApiErrorCode::GameHandleOperationTypeError;
			errmsg = "type invalid";
			break;
		}
		//2.上分 3.下分
		if (opType != int(eApiType::OpAddScore) && opType != int(eApiType::OpSubScore)) {
			// 操作类型参数错误
			errcode = eApiErrorCode::GameHandleOperationTypeError;
			errmsg = "type value invalid";
			break;
		}
		//agentid
		HttpParams::const_iterator agentIdKey = params.find("agentid");
		if (agentIdKey == params.end() || agentIdKey->second.empty() ||
			(agentId = atol(agentIdKey->second.c_str())) <= 0) {
			// 传输参数错误
			errcode = eApiErrorCode::GameHandleParamsError;
			errmsg = "agentid invalid";
			break;
		}
		//timestamp
		HttpParams::const_iterator timestampKey = params.find("timestamp");
		if (timestampKey == params.end() || timestampKey->second.empty() ||
			atol(timestampKey->second.c_str()) <= 0) {
			// 传输参数错误
			errcode = eApiErrorCode::GameHandleParamsError;
			errmsg = "timestamp invalid";
			break;
		}
		else {
			timestamp = timestampKey->second;
		}
		//paraValue
		HttpParams::const_iterator paramValueKey = params.find("paraValue");
		if (paramValueKey == params.end() || paramValueKey->second.empty()) {
			// 传输参数错误
			errcode = eApiErrorCode::GameHandleParamsError;
			errmsg = "paraValue invalid";
			break;
		}
		else {
			paraValue = paramValueKey->second;
		}
		//key
		HttpParams::const_iterator keyKey = params.find("key");
		if (keyKey == params.end() || keyKey->second.empty()) {
			// 传输参数错误
			errcode = eApiErrorCode::GameHandleParamsError;
			errmsg = "key invalid";
			break;
		}
		else {
			key = keyKey->second;
		}
		// 获取当前代理数据
		//agent_info_t _agent_info = { 0 };
		{
			READ_LOCK(agent_info_mutex_);
			std::map<int32_t, agent_info_t>::/*const_*/iterator it = agent_info_.find(agentId);
			if (it == agent_info_.end()) {
				// 代理ID不存在或代理已停用
				errcode = eApiErrorCode::GameHandleProxyIDError;
				errmsg = "agent_info not found";
				break;
			}
			else {
				p_agent_info = &it->second;
			}
		}
		// 没有找到代理，判断代理的禁用状态(0正常 1停用)
		if (p_agent_info->status == 1) {
			// 代理ID不存在或代理已停用
			errcode = eApiErrorCode::GameHandleProxyIDError;
			errmsg = "agent.status error";
			break;
		}
#if 0
		agentId = 10000;
		p_agent_info->md5code = "334270F58E3E9DEC";
		p_agent_info->descode = "111362EE140F157D";
		timestamp = "1579599778583";
		paraValue = "0RJ5GGzw2hLO8AsVvwORE2v16oE%2fXSjaK78A98ct5ajN7reFMf9YnI6vEnpttYHK%2fp04Hq%2fshp28jc4EIN0aAFeo4pni5jxFA9vg%2bLOx%2fek%3d";
		key = "C6656A2145BAEF7ED6D38B9AF2E35442";
#elif 0
		agentId = 111169;
		p_agent_info->md5code = "8B56598D6FB32329";
		p_agent_info->descode = "D470FD336AAB17E4";
		timestamp = "1580776071271";
		paraValue = "h2W2jwWIVFQTZxqealorCpSfOwlgezD8nHScU93UQ8g%2FDH1UnoktBusYRXsokDs8NAPFEG8WdpSe%0AY5rtksD0jw%3D%3D";
		key = "a7634b1e9f762cd4b0d256744ace65f0";
#elif 0
		agentId = 111190;
		timestamp = "1583446283986";
		p_agent_info->md5code = "728F0884A000FD72";
		p_agent_info->descode = "AAFFF4393E17DB6B";
		paraValue = "KDtKjjnaaxKWNuK%252BBRwv9f2gBxLkSvY%252FqT4HBaZY2IrxqGMK3DYlWOW4dHgiMZV8Uu66h%252BHjH8MfAfpQIE5eIHoRZMplj7dljS7Tfyf3%252BlM%253D";
		key = "4F6F53FDC4D27EC33B3637A656DD7C9F";
#elif 0
		agentId = 111149;
		timestamp = "1583448714906";
		p_agent_info->md5code = "7196FD6921347DB1";
		p_agent_info->descode = "A5F8C07B7843C73E";
		paraValue = "nu%252FtdBhN6daQdad3aOVOgzUr6bHVMYNEpWE4yLdHkKRn%252F%252Fn1V3jIFR%252BI7wawXWNyjR3%252FW0M9qzcdzM8rNx8xwe%252BEW9%252BM92ZN4hlpUAhFH74%253D";
		key = "9EEC240FAFAD3E5B6AB6B2DDBCDFE1FF";
#elif 0
		agentId = 10033;
		timestamp = "1583543899005";
		p_agent_info->md5code = "5998F124C180A817";
		p_agent_info->descode = "38807549DEA3178D";
		paraValue = "9303qk%2FizHRAszhN33eJxG2aOLA2Wq61s9f96uxDe%2Btczf2qSG8O%2FePyIYhVAaeek9m43u7awgra%0D%0Au8a8b%2FDchcZSosz9SVfPjXdc7h4Vma2dA8FHYZ5dJTcxWY7oDLlSOHKVXYHFMIWeafVwCN%2FU5fzv%0D%0AHWyb1Oa%2FWJ%2Bnfx7QXy8%3D";
		key = "2a6b55cf8df0cd8824c1c7f4308fd2e4";
#endif
		//解码 ///
		std::string decrypt;
		{
			//根据代理商信息中存储的md5密码，结合传输参数中的agentid和timestamp，生成判定标识key
			std::string src = std::to_string(agentId) + timestamp + p_agent_info->md5code;
			char md5[32 + 1] = { 0 };
			MD5Encode32(src.c_str(), src.length(), md5, 1);
			if (strncasecmp(md5, key.c_str(), std::min<size_t>(32, key.length())) != 0) {
				// 代理MD5校验码错误
				errcode = eApiErrorCode::GameHandleProxyMD5CodeError;
				errmsg = "md5 check error";
				break;
			}

			//HTML::Decode ///
			//paraValue = HTML::Decode(paraValue);
			//LOG_DEBUG << "--- *** " << "HTML::Decode >>> " << paraValue;

			//UrlDecode 1次或2次解码 ///
			for (int c = 1; c < 3; ++c) {
#if 1
				/*std::string strURL*/paraValue = URL::Decode(paraValue);
#else
				/*std::string strURL*/paraValue = URL::Decode2(paraValue);
#endif
				//"\r\n"
				boost::replace_all<std::string>(paraValue, "\r\n", "");
				//"\r"
				boost::replace_all<std::string>(paraValue, "\r", "");
				//"\r\n"
				boost::replace_all<std::string>(paraValue, "\n", "");

				LOG_DEBUG << "--- *** " << "UrlDecode >>> " << paraValue/*strURL*/;

				std::string const& strURL = paraValue;
				decrypt = Crypto::AES_ECBDecrypt(strURL, p_agent_info->descode);
				LOG_DEBUG << "--- *** " << "ECBDecrypt[" << c
					<< "] >>> md5code[" << p_agent_info->md5code
					<< "] descode[" << p_agent_info->descode << "] [" << decrypt << "]";

				if (!decrypt.empty()) {
					//成功
					break;
				}
			}

			//Base64Decode ///
			//std::string strBase64 = Base64::Decode(strURL);
			//LOG_DEBUG << "--- *** " << "base64Decode\n" << strBase64;

			//AES校验 ///
#if 0
			//decrypt = Landy::Crypto::ECBDecrypt(p_agent_info->descode, (unsigned char const*)strURL.c_str());
			//"\006\006\006\006\006\006"
			//boost::replace_all<std::string>(decrypt, "\006", "");
#endif
			//decrypt = Crypto::AES_ECBDecrypt(strURL, p_agent_info->descode);
			//LOG_DEBUG << "--- *** " << "ECBDecrypt >>> " << decrypt;

			if (decrypt.empty()) {
				// 参数转码或代理解密校验码错误
				errcode = eApiErrorCode::GameHandleProxyDESCodeError;
				errmsg = "DESDecrypt failed, AES_set_decrypt_key error";
				break;
			}
		}
		//解析内部参数 ///
		{
			HttpParams decryptParams;
			if (!parseQuery(decrypt, decryptParams, errmsg)) {
				// 传输参数错误
				errcode = eApiErrorCode::GameHandleParamsError;
				errmsg = "DESDecrypt ok, but bad request paramValue";
				break;
			}
			//agentid
			//HttpParams::const_iterator agentIdKey = decryptParams.find("agentid");
			//if (agentIdKey == decryptParams.end() || agentIdKey->second.empty()) {
			//	break;
			//}
#ifdef _NO_LOGIC_PROCESS_
			//userid
	 		HttpParams::const_iterator userIdKey = decryptParams.find("userid");
	 		if (userIdKey == decryptParams.end() || userIdKey->second.empty() ||
	 			(userId = atoll(userIdKey->second.c_str())) <= 0) {
				errcode = eApiErrorCode::GameHandleParamsError;
	 			errmsg += "userid ";
	 		}
#endif
			//account
			HttpParams::const_iterator accountKey = decryptParams.find("account");
			if (accountKey == decryptParams.end() || accountKey->second.empty()) {
				// 传输参数错误
				errcode = eApiErrorCode::GameHandleParamsError;
				errmsg += "account ";
			}
			else {
				account = accountKey->second;
			}
			//orderid
			HttpParams::const_iterator orderIdKey = decryptParams.find("orderid");
			if (orderIdKey == decryptParams.end() || orderIdKey->second.empty()) {
				// 传输参数错误
				errcode = eApiErrorCode::GameHandleParamsError;
				errmsg += "orderid ";
			}
			else {
				orderId = orderIdKey->second;
			}
			//score
			HttpParams::const_iterator scoreKey = decryptParams.find("score");
			if (scoreKey == decryptParams.end() || scoreKey->second.empty() || !IsDigitStr(scoreKey->second) ||
				(score = atoll(scoreKey->second.c_str())) <= 0) {
				// 传输参数错误
				errcode = eApiErrorCode::GameHandleParamsError;
				errmsg += "score ";
			}
			//有错误发生 ///
			if (errcode != 0) {
				errmsg += "invalid";
				break;
			}
		}
#ifndef _NO_LOGIC_PROCESS_
		//上下分操作 ///
		errcode = doOrderExecute(opType, account, score, *p_agent_info, orderId, errmsg, latest, testTPS);
#endif
	} while (0);
	
	//调试模式下，打印从接收网络请求(receive)到处理完逻辑业务所经历时间dt(s) ///
	char ch[50] = { 0 };
	snprintf(ch, sizeof(ch), " dt(%.6fs)", muduo::timeDifference(muduo::Timestamp::now(), receiveTime));
	errmsg += ch;
	//json格式应答消息 ///
	std::string json = createResponse(opType, orderId, agentId, account, score, errcode, errmsg, g_bisDebug);
	/*LOG_WARN*/LOG_DEBUG << "--- *** " << "\n" << json;
	
	return json;
}

//上下分操作 ///
int ApiServer::doOrderExecute(int32_t opType, std::string const& account, int64_t score, agent_info_t& _agent_info, std::string const& orderId, std::string& errmsg, boost::property_tree::ptree& latest, int& testTPS)
{
	int errcode = eApiErrorCode::NoError;
	do {
		//上分操作
		if (opType == int(eApiType::OpAddScore)) {

			/*LOG_WARN*/LOG_INFO << __FUNCTION__ << " --- *** " << "上分REQ "
				<< "orderId[" << orderId << "] "
				<< "account[" << account << "]" << " agentId[" << _agent_info.agentId << "]"
				<< " deltascore[" << score << "]";

			//处理上分请求 ///
			errcode = AddOrderScore(account, score * 100, _agent_info, orderId, errmsg, latest, testTPS);

			/*LOG_WARN*/LOG_INFO << __FUNCTION__ << " --- *** " << "上分RSP "
				<< "\norderId[" << orderId << "] " << "account[" << account << "] status[" << errcode << "] errmsg[" << errmsg << "]";
		}
		//下分操作
		else /*if (opType == int(eApiType::OpSubScore))*/ {

			/*LOG_WARN*/LOG_INFO << __FUNCTION__ << " --- *** " << "下分REQ "
				<< "orderId[" << orderId << "] "
				<< "account[" << account << "]" << " agentId[" << _agent_info.agentId << "]"
				<< " deltascore[" << score << "]";

			//处理下分请求 ///
			errcode = SubOrderScore(account, score * 100, _agent_info, orderId, errmsg, latest, testTPS);

			/*LOG_WARN*/LOG_INFO << __FUNCTION__ << " --- *** " << "下分RSP "
				<< "orderId[" << orderId << "] " << "account[" << account << "] status[" << errcode << "] errmsg[" << errmsg << "]";
		}
	} while (0);
	return errcode;
}

//上分写db操作 ///
int ApiServer::AddOrderScore(std::string const& account, int64_t score, agent_info_t& _agent_info, std::string const& orderId,
	std::string& errmsg, boost::property_tree::ptree& latest, int& testTPS)
{
	return 0;
}

//下分写db操作 ///
int ApiServer::SubOrderScore(std::string const& account, int64_t score, agent_info_t& _agent_info, std::string const& orderId
	, std::string& errmsg, boost::property_tree::ptree& latest, int& testTPS)
{
	return 0;
}
