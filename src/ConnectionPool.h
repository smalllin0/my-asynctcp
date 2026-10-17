#ifndef MY_ASYNC_TCP_CONNECTION_POOL_H_
#define MY_ASYNC_TCP_CONNECTION_POOL_H_

#include "AsyncConnection.h"
#include "ObjPool.h"

using ConnectionPool = ObjPool<AsyncConnection>;


#endif /* MY_ASYNC_TCP_CONNECTION_POOL_H_ */