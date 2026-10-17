#ifndef MY_ASYNC_TCP_POOL_NODE_H_
#define MY_ASYNC_TCP_POOL_NODE_H_


template<typename Derived>
struct PoolNode {
    Derived* next{nullptr};
};



#endif // MY_ASYNC_TCP_POOL_NODE_H_