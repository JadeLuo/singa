#ifndef INCLUDE_CORE_GLOBAL_TABLE_H_
#define INCLUDE_CORE_GLOBAL_TABLE_H_

#include <glog/logging.h>
#include "core/table.h"
#include "core/local-table.h"
#include "core/file.h"
#include "core/request_dispatcher.h"

namespace lapis {

class TableServer;

class GlobalTable :
  public TableBase {
 public:
  virtual void Init(const TableDescriptor *tinfo);
  virtual ~GlobalTable();

  struct PartitionInfo {
    PartitionInfo() : owner(-1) {}
    int owner;
  };

  virtual PartitionInfo *get_partition_info(int shard) {
    return &partinfo_[shard];
  }

  int owner(int shard) {
    return get_partition_info(shard)->owner;
  }

  LocalTable *get_partition(int shard);

  bool is_local_shard(int shard);
  bool is_local_key(const StringPiece &k);

  // Fill in a response from a remote worker for the given key.
  void handle_get(const HashGet &req, TableData *resp);

  // Handle updates from the master or other workers.
  void SendUpdates();
  void ApplyUpdates(const TableData &req);
  void UpdatePartitions(const ShardInfo &sinfo);

  Stats stats();

  int pending_write_bytes();

  // Clear any local data for which this table has ownership.
  // Updates waiting to be sent to other workers are *not* cleared.
  void clear(int shard);
  bool empty();
  void resize(int64_t new_size);

  int worker_id_;

  virtual int64_t shard_size(int shard);

  virtual int get_shard_str(StringPiece k) = 0;
 protected:
  vector<PartitionInfo> partinfo_;
  boost::recursive_mutex &mutex() {
    return m_;
  }
  vector<LocalTable *> partitions_;
  vector<LocalTable *> cache_;

  volatile int pending_writes_;
  boost::recursive_mutex m_;

  friend class TableServer;
  TableServer *w_;


  void set_worker(TableServer *w);

  // Fetch key k from the node owning it.  Returns true if the key exists.
  bool get_remote(int shard, const StringPiece &k, string *v);

  // send get request to the remote machine.
  void async_get_remote(int shard, const StringPiece &k);

  //  true if there're responses to GET request
  bool async_get_remote_collect(string *k, string *v);
};

template <class K, class V>
class TypedGlobalTable :
  public GlobalTable,
  public TypedTable<K, V>,
  private boost::noncopyable {
 public:
  virtual void Init(const TableDescriptor *tinfo) {
    GlobalTable::Init(tinfo);
    for (size_t i = 0; i < partitions_.size(); ++i) {
      partitions_[i] = create_local(i);
    }
    pending_writes_ = 0;
  }

  int get_shard(const K &k);
  int get_shard_str(StringPiece k);
  V get_local(const K &k);

  // Store the given key-value pair in this hash. If 'k' has affinity for a
  // remote thread, the application occurs immediately on the local host,
  // and the update is queued for transmission to the owner.
  void put(const K &k, const V &v);
  void update(const K &k, const V &v);

  // Return the value associated with 'k', possibly blocking for a remote fetch.
  V get(const K &k);

  //  asynchronous get. if true (local data), then V contains the value
  //  if false, use has to call async_collect() until there's data (returning true)
  bool async_get(const K &k, V* v);
  bool async_get_collect(K* k, V* v);

  bool contains(const K &k);
  void remove(const K &k);

  TypedTable<K, V> *partition(int idx) {
    return dynamic_cast<TypedTable<K, V>*>(partitions_[idx]);
  }

 protected:
  LocalTable *create_local(int shard);
};


template<class K, class V>
int TypedGlobalTable<K, V>::get_shard(const K &k) {
  DCHECK(this != NULL);
  DCHECK(this->info().sharder != NULL);
  Sharder<K> *sharder = (Sharder<K> *)(this->info().sharder);
  int shard = (*sharder)(k, this->info().num_shards);
  DCHECK_GE(shard, 0);
  DCHECK_LT(shard, this->num_shards());
  return shard;
}

template<class K, class V>
int TypedGlobalTable<K, V>::get_shard_str(StringPiece k) {
  return get_shard(unmarshal(static_cast<Marshal<K>*>(this->info().key_marshal),
                             k));
}

template<class K, class V>
V TypedGlobalTable<K, V>::get_local(const K &k) {
  int shard = this->get_shard(k);
  return partition(shard)->get(k);
}

// Store the given key-value pair in this hash. If 'k' has affinity for a
// remote thread, the application occurs immediately on the local host,
// and the update is queued for transmission to the owner.
template<class K, class V>
void TypedGlobalTable<K, V>::put(const K &k, const V &v) {
  int shard = this->get_shard(k);
  VLOG(3)<<"sent to shard: "<<shard;
  //  boost::recursive_mutex::scoped_lock sl(mutex());
  string key = marshal(static_cast<Marshal<K>*>(this->info().key_marshal), k);
  if (is_local_shard(shard))
	  RequestDispatcher::Get()->sync_local_put(key);

  partition(shard)->put(k, v);

  if (is_local_shard(shard))
	  RequestDispatcher::Get()->event_complete(key);

  //  always send
  if (!is_local_shard(shard)) {
    SendUpdates();
  }
}

template<class K, class V>
void TypedGlobalTable<K, V>::update(const K &k, const V &v) {
  int shard = this->get_shard(k);
  string key = marshal(static_cast<Marshal<K>*>(this->info().key_marshal), k);
  //  boost::recursive_mutex::scoped_lock sl(mutex());
  if (is_local_shard(shard))
	  RequestDispatcher::Get()->sync_local_put(key);

  partition(shard)->update(k, v);

  if (is_local_shard(shard))
	  RequestDispatcher::Get()->event_complete(key);

  //  always send
  if (!is_local_shard(shard)){
    SendUpdates();
  }
}

// Return the value associated with 'k', possibly blocking for a remote fetch.
template<class K, class V>
V TypedGlobalTable<K, V>::get(const K &k) {
  int shard = this->get_shard(k);
  string key = marshal(static_cast<Marshal<K>*>(this->info().key_marshal), k);

  if (is_local_shard(shard)) {
	RequestDispatcher::Get()->sync_local_get(key);
    V val =  get_local(k);
    RequestDispatcher::Get()->event_complete(key);
    return val;
  }
  string v_str;
  get_remote(shard,key, &v_str);
  return unmarshal(static_cast<Marshal<V>*>(this->info().value_marshal), v_str);
}

/*
 * asynchronous get, returns right away. use async_get_collect to wait on the
 * content.
 *
 * if the key is local, return
 * else send send (get_remote without waiting).
 */
template<class K, class V>
bool TypedGlobalTable<K,V>::async_get(const K &k, V* v){
  int shard = this->get_shard(k);

  if (is_local_shard(shard)) return false;
  else{
		async_get_remote(shard,
				marshal(static_cast<Marshal<K>*>(this->info().key_marshal), k));
		return false;
  }

}

/*
 * wait on the content. The upper layer repeatedly invokes this function until
 * it returns true, then the value can be used.
 *
 * K k;
 * V v;
 * if (async_get_collect(&k,&v))
 *     do_some_thing(k,v);
 */
template<class K, class V>
bool TypedGlobalTable<K,V>::async_get_collect(K* k, V* v){
	if (is_local_shard(this->get_shard(*k))){
		  string key_str= marshal(static_cast<Marshal<K>*>(this->info().key_marshal), *k);
		if (RequestDispatcher::Get()->table_queue()->sync_local_get(key_str)){
			*v = get_local(*k);
			RequestDispatcher::Get()->event_complete(key_str);
			return true;
		}
		else
			return false;
	}
	string tk, tv;
	bool succeed = async_get_remote_collect(&tk, &tv);
	if (succeed){
		*k = unmarshal(static_cast<Marshal<K>*>(this->info().key_marshal), tk);
		*v = unmarshal(static_cast<Marshal<V>*>(this->info().value_marshal), tv);
	}
	return succeed;
}


template<class K, class V>
bool TypedGlobalTable<K, V>::contains(const K &k) {
  int shard = this->get_shard(k);
  if (is_local_shard(shard)) {
    //    boost::recursive_mutex::scoped_lock sl(mutex());
    return partition(shard)->contains(k);
  }
  string v_str;
  return get_remote(shard, marshal(static_cast<Marshal<K>*>(info_->key_marshal),
                                   k), &v_str);
}

template<class K, class V>
void TypedGlobalTable<K, V>::remove(const K &k) {
  LOG(FATAL) << "Not implemented!";
}

template<class K, class V>
LocalTable *TypedGlobalTable<K, V>::create_local(int shard) {
  TableDescriptor *linfo = new TableDescriptor(info());
  linfo->shard = shard;
  LocalTable *t = (LocalTable *)info_->partition_factory->New();
  t->Init(linfo);
  return t;
}

}  // namespace lapis

#endif /* INCLUDE_CORE_GLOBAL_TABLE_H_ */
