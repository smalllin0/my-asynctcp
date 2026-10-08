# my-asynctcp

## to-do

- 向新的架构迁移
  1. 先是变更接口到新的模式：大驼峰
  2. 而后再使用LwipWrapper包装底层操作
  3. 加入连接状态ConnectionState
  4. 完善No Copy接口（与Copy接口并存复杂），需保证数据传入lwip一致性、ack的一致性。因此要与no copy一同保存状态