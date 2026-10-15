// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
// One producer, one consumer. Close follows producer quiescence; it can also
// cancel a producer blocked on a full queue. The consumer drains published work.
// A returned Front pointer remains valid until CompleteFront; completion is
// published only AFTER the command has finished touching the command buffer.
template<class T,uint32_t Capacity,uint32_t Batch=32> class BlockingRecordQueue {
 static_assert(Capacity && !(Capacity&(Capacity-1)) && Capacity<0x80000000u);
 static_assert(Batch && Batch<=Capacity);
 std::unique_ptr<T[]> storage_=std::make_unique<T[]>(Capacity);
 std::atomic<uint32_t> head_{0},tail_{0},target_{0};
 std::atomic<bool> closed_{false},consumer_sleeping_{false},producer_waiting_{false};
 mutable std::mutex mutex_;
 std::condition_variable work_cv_,done_cv_;
public:
 struct WaitStats {uint64_t producer_parks=0,consumer_parks=0,completion_notifications=0;};
private:
 WaitStats stats_{}; // accessed only under mutex_
 static bool Reached(uint32_t cursor,uint32_t target){return uint32_t(cursor-target)<0x80000000u;}
 bool WaitUntil(uint32_t target,bool cancel_on_close) {
  std::unique_lock lock(mutex_);
  if(Reached(tail_.load(std::memory_order_seq_cst),target))return !cancel_on_close || !closed_.load(std::memory_order_seq_cst);
  target_.store(target,std::memory_order_relaxed);
  producer_waiting_.store(true,std::memory_order_seq_cst);
  work_cv_.notify_one();
  ++stats_.producer_parks;
  done_cv_.wait(lock,[&]{return Reached(tail_.load(std::memory_order_seq_cst),target) || (cancel_on_close && closed_.load(std::memory_order_seq_cst));});
  producer_waiting_.store(false,std::memory_order_seq_cst);
  return !cancel_on_close || !closed_.load(std::memory_order_seq_cst);
 }
public:
 // Initial cursor is for testing rollover; production starts at zero.
 explicit BlockingRecordQueue(uint32_t initial=0):head_(initial),tail_(initial){}
 bool Push(const T& value) {
  if(closed_.load(std::memory_order_seq_cst))return false;
  const uint32_t head=head_.load(std::memory_order_relaxed);
  if(head-tail_.load(std::memory_order_acquire)>=Capacity && !WaitUntil(head-Capacity+1,true))return false;
  storage_[head&(Capacity-1)]=value;
  head_.store(head+1,std::memory_order_seq_cst);
  if(head+1-tail_.load(std::memory_order_relaxed)>=Batch)WakeConsumer();
  return true;
 }
 const T* FrontOrWait() {
  for(;;){
   const uint32_t tail=tail_.load(std::memory_order_relaxed);
   if(tail!=head_.load(std::memory_order_seq_cst))return &storage_[tail&(Capacity-1)];
   std::unique_lock lock(mutex_);
   consumer_sleeping_.store(true,std::memory_order_seq_cst);
   if(tail==head_.load(std::memory_order_seq_cst) && !closed_.load(std::memory_order_seq_cst)){
    ++stats_.consumer_parks;
    work_cv_.wait(lock,[&]{return tail!=head_.load(std::memory_order_seq_cst) || closed_.load(std::memory_order_seq_cst);});
   }
   consumer_sleeping_.store(false,std::memory_order_seq_cst);
   if(tail==head_.load(std::memory_order_seq_cst) && closed_.load(std::memory_order_seq_cst))return nullptr;
  }
 }
 void CompleteFront() {
  const uint32_t tail=tail_.load(std::memory_order_relaxed)+1;
  tail_.store(tail,std::memory_order_seq_cst);
  if(producer_waiting_.load(std::memory_order_seq_cst) && Reached(tail,target_.load(std::memory_order_relaxed))){
   std::lock_guard lock(mutex_);
   // Recheck under lock: an old completion cannot clear a newer wait request.
   if(producer_waiting_.load(std::memory_order_seq_cst) && Reached(tail_.load(std::memory_order_seq_cst),target_.load(std::memory_order_relaxed))){
    producer_waiting_.store(false,std::memory_order_seq_cst);
    ++stats_.completion_notifications;
    done_cv_.notify_one();
   }
  }
 }
 void WakeConsumer() {
  // SC publication/sleep flags guarantee either the predicate sees new work
  // or its publisher sees sleeping=true and notifies under the same mutex.
  if(consumer_sleeping_.load(std::memory_order_seq_cst)){
   std::lock_guard lock(mutex_);work_cv_.notify_one();
  }
 }
 bool Pending()const{return head_.load(std::memory_order_acquire)!=tail_.load(std::memory_order_acquire);}
 void Drain(){const uint32_t head=head_.load(std::memory_order_relaxed);if(tail_.load(std::memory_order_acquire)!=head)WaitUntil(head,false);}
 void Close(){closed_.store(true,std::memory_order_seq_cst);std::lock_guard lock(mutex_);work_cv_.notify_one();done_cv_.notify_one();}
 WaitStats GetWaitStats()const{std::lock_guard lock(mutex_);return stats_;}
};