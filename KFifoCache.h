#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

#include "KICachePolicy.h"

namespace Cache
{

// 前向声明
template<typename Key, typename Value> class KFifoCache;

// FIFO节点结构
template<typename Key, typename Value>
class FifoNode 
{
private:
    Key key_;
    Value value_;
    std::weak_ptr<FifoNode<Key, Value>> prev_;  // 前一个节点
    std::shared_ptr<FifoNode<Key, Value>> next_; // 后一个节点

public:
    FifoNode(Key key, Value value)
        : key_(key)
        , value_(value)
    {}

    // 提供必要的访问器
    Key getKey() const { return key_; }
    Value getValue() const { return value_; }
    void setValue(const Value& value) { value_ = value; }

    friend class KFifoCache<Key, Value>;
};

// FIFO缓存实现
template<typename Key, typename Value>
class KFifoCache : public KICachePolicy<Key, Value>
{
public:
    using FifoNodeType = FifoNode<Key, Value>;
    using NodePtr = std::shared_ptr<FifoNodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

    // 构造函数
    KFifoCache(int capacity)
        : capacity_(capacity)
    {
        initializeQueue();
    }

    ~KFifoCache() override = default;

    // 添加缓存
    void put(Key key, Value value) override
    {
        if (capacity_ <= 0)
            return;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // 如果键已存在，更新值
            updateExistingNode(it->second, value);
            return;
        }

        addNewNode(key, value);
    }

    // 获取缓存
    bool get(Key key, Value& value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // FIFO不需要移动节点位置，直接返回值
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    // 获取缓存（重载版本）
    Value get(Key key) override
    {
        Value value{};
        get(key, value);
        return value;
    }

    // 删除指定元素
    void remove(Key key) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            removeNode(it->second);
            nodeMap_.erase(it);
        }
    }

private:
    // 初始化队列
    void initializeQueue()
    {
        // 创建首尾虚拟节点
        dummyHead_ = std::make_shared<FifoNodeType>(Key(), Value());
        dummyTail_ = std::make_shared<FifoNodeType>(Key(), Value());
        dummyHead_->next_ = dummyTail_;
        dummyTail_->prev_ = dummyHead_;
    }

    // 更新现有节点
    void updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);
        // FIFO不需要移动节点位置
    }

    // 添加新节点
    void addNewNode(const Key& key, const Value& value) 
    {
        // 如果缓存已满，淘汰最早进入的节点
        if (nodeMap_.size() >= capacity_)
        {
            evictOldest();
        }

        // 创建新节点并添加到队尾
        NodePtr newNode = std::make_shared<FifoNodeType>(key, value);
        enqueueNode(newNode);
        nodeMap_[key] = newNode;
    }

    // 将节点入队（添加到队尾）
    void enqueueNode(NodePtr node) 
    {
        // 将节点添加到队尾（dummyTail_之前）
        node->prev_ = dummyTail_->prev_;
        node->next_ = dummyTail_;
        dummyTail_->prev_.lock()->next_ = node;
        dummyTail_->prev_ = node;
    }

    // 从队列中移除节点
    void removeNode(NodePtr node) 
    {
        if(!node->prev_.expired() && node->next_) 
        {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
            node->next_->prev_ = prev;
            node->next_ = nullptr;
        }
    }

    // 淘汰最早进入的节点（队首元素）
    void evictOldest() 
    {
        // 最早进入的节点在队首（dummyHead_->next_）
        NodePtr oldestNode = dummyHead_->next_;
        removeNode(oldestNode);
        nodeMap_.erase(oldestNode->getKey());
    }

private:
    int           capacity_; // 缓存容量
    NodeMap       nodeMap_; // key -> Node 映射
    std::mutex    mutex_;   // 互斥锁，保证线程安全
    NodePtr       dummyHead_; // 虚拟头节点
    NodePtr       dummyTail_; // 虚拟尾节点
};

// FIFO优化：分片式FIFO缓存，提高并发性能
template<typename Key, typename Value>
class KHashFifoCache
{
public:
    KHashFifoCache(size_t capacity, int sliceNum)
        : capacity_(capacity)
        , sliceNum_(sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency())
    {
        size_t sliceSize = std::ceil(capacity_ / static_cast<double>(sliceNum_)); // 每个分片的容量
        for (int i = 0; i < sliceNum_; ++i)
        {
            fifoSliceCaches_.emplace_back(new KFifoCache<Key, Value>(sliceSize));
        }
    }

    // 添加缓存
    void put(Key key, Value value)
    {
        // 根据key找出对应的分片
        size_t sliceIndex = Hash(key) % sliceNum_;
        fifoSliceCaches_[sliceIndex]->put(key, value);
    }

    // 获取缓存
    bool get(Key key, Value& value)
    {
        // 根据key找出对应的分片
        size_t sliceIndex = Hash(key) % sliceNum_;
        return fifoSliceCaches_[sliceIndex]->get(key, value);
    }

    // 获取缓存（重载版本）
    Value get(Key key)
    {
        Value value{};
        get(key, value);
        return value;
    }

private:
    // 将key计算成对应哈希值
    size_t Hash(Key key)
    {
        std::hash<Key> hashFunc;
        return hashFunc(key);
    }

private:
    size_t capacity_; // 缓存总容量
    int sliceNum_; // 缓存分片数量
    std::vector<std::unique_ptr<KFifoCache<Key, Value>>> fifoSliceCaches_; // FIFO分片容器
};

} // namespace Cache
