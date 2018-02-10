/*
Copyright 2017, object_he@yeah.net  All rights reserved.

Author: object_he@yeah.net

Last modified: 2017-11-8

Description:
*/

#include "uv/TcpConnection.h"
#include "uv/TcpServer.h"
#include "uv/Async.h"
#include "uv/LogInterface.h"

using namespace std;
using namespace std::chrono;
using namespace uv;

struct WriteReq
{
    uv_write_t req;
    uv_buf_t buf;
    AfterWriteCallback callback;
};

struct WriteArgs
{
    WriteArgs(shared_ptr<TcpConnection> conn = nullptr, const char* buf = nullptr, ssize_t size = 0, AfterWriteCallback callback = nullptr)
        :connection(conn),
        buf(buf),
        size(size),
        callback(callback)
    {

    }
    shared_ptr<TcpConnection> connection;
    const char* buf;
    ssize_t size;
    AfterWriteCallback callback;
};

TcpConnection:: ~TcpConnection()
{
    delete handle_;
}

TcpConnection::TcpConnection(EventLoop* loop, std::string& name, uv_tcp_t* client, bool isConnected)
    :name_(name),
    connected_(isConnected),
    loop_(loop),
    handle_(client),
    onMessageCallback_(nullptr),
    onConnectCloseCallback_(nullptr),
    closeCompleteCallback_(nullptr)
{
    handle_->data = static_cast<void*>(this);
    ::uv_read_start((uv_stream_t*)client,
        [](uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
    {
        buf->base = new char[suggested_size];
#if _MSC_VER
        buf->len = (ULONG)suggested_size;
#else
        buf->len = suggested_size;
#endif
    },
        &TcpConnection::onMesageReceive);
}




void TcpConnection::onMessage(const char* buf, ssize_t size)
{
    if (onMessageCallback_)
        onMessageCallback_(shared_from_this(), buf, size);
}

void TcpConnection::onSocketClose()
{
    if (onConnectCloseCallback_)
        onConnectCloseCallback_(name_);
}

void TcpConnection::close(std::function<void(std::string&)> callback)
{
    if (uv_is_active((uv_handle_t*)handle_)) 
    {
        uv_read_stop((uv_stream_t*)handle_);
    }
    if (uv_is_closing((uv_handle_t*)handle_) == 0)
    {
        //libuv 在loop轮询中会检测关闭句柄，delete会导致程序异常退出。
        ::uv_close((uv_handle_t*)handle_,
            [](uv_handle_t* handle)
        {
            auto connection = static_cast<TcpConnection*>(handle->data);
            connection->CloseComplete();
        });
    }
}

int TcpConnection::write(const char* buf, ssize_t size, AfterWriteCallback callback)
{
    int rst;
    if (connected_)
    {
        WriteReq* req = new WriteReq;
        req->buf = uv_buf_init(const_cast<char*>(buf), static_cast<unsigned int>(size));
        req->callback = callback;
        rst = ::uv_write((uv_write_t*)req, (uv_stream_t*)handle_, &req->buf, 1,
            [](uv_write_t *req, int status)
        {
            WriteReq *wr = (WriteReq*)req;
            if (nullptr != wr->callback)
            {
                struct WriteInfo info;
                info.buf = const_cast<char*>(wr->buf.base);
                info.size = wr->buf.len;
                info.status = status;
                wr->callback(info);
            }
            //delete [] (wr->buf.base);
            delete wr;
        });
    }
    else
    {
        rst = -1;
        if (nullptr != callback)
        {
            struct WriteInfo info;
            info.buf = const_cast<char*>(buf);
            info.size = static_cast<unsigned long>(size);
            info.status = WriteInfo::Disconnected;
            callback(info);
        }
    }

    return rst;
}

void TcpConnection::writeInLoop(const char* buf, ssize_t size, AfterWriteCallback callback)
{
    if (loop_->isRunInLoopThread())
    {
        write(buf, size, callback);
        return;
    }

    Async<struct WriteArgs>* async = new Async<struct WriteArgs>(loop_,
    std::bind([this](Async<struct WriteArgs>* handle, struct WriteArgs * data)
    {
        auto connection = data->connection;
        connection->write(data->buf, data->size, data->callback);
        delete data;
        handle->close();
        delete handle;
    },
    std::placeholders::_1, std::placeholders::_2));

    struct WriteArgs* writeArg = new struct WriteArgs(shared_from_this(), buf, size, callback);

    async->setData(writeArg);
    async->runInLoop();
}


void TcpConnection::setElement(shared_ptr<ConnectionElement> conn)
{
    element_ = conn;
}

std::weak_ptr<ConnectionElement> TcpConnection::Element()
{
    return element_;
}

void  TcpConnection::onMesageReceive(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf)
{
    auto connection = static_cast<TcpConnection*>(client->data);
    if (nread > 0)
    {
        connection->onMessage(buf->base, nread);
        delete[](buf->base);
    }
    else if (nread < 0)
    {
        connection->setConnectStatus(false);
        uv::Log::Instance()->error( uv_err_name((int)nread));
        delete[](buf->base);

        if (nread != UV_EOF)
        {
            connection->onSocketClose();
            return;
        }

        uv_shutdown_t* sreq = new uv_shutdown_t;
        sreq->data = static_cast<void*>(connection);
        ::uv_shutdown(sreq, (uv_stream_t*)client,
            [](uv_shutdown_t* req, int status)
        {
            auto connection = static_cast<TcpConnection*>(req->data);
            connection->onSocketClose();
            delete req;
        });
    }
    else
    {
        /* Everything OK, but nothing read. */
        delete[](buf->base);
    }

}
