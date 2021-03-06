/**
 * virtual_table.h
 */

#pragma once

#include "buffer/lru_replacer.h"
#include "catalog/schema.h"
#include "index/b_plus_tree_index.h"
#include "sqlite/sqlite3ext.h"
#include "table/table_heap.h"
#include "table/tuple.h"
#include "type/value.h"

namespace cmudb {
/* Helpers */
Schema *ParseCreateStatement(const std::string &sql);

IndexMetadata *ParseIndexStatement(std::string &sql,
                                   const std::string &table_name,
                                   Schema *schema);

Tuple ConstructTuple(Schema *schema, sqlite3_value **argv);

Index *ConstructIndex(IndexMetadata *metadata,
                      BufferPoolManager *buffer_pool_manager,
                      page_id_t root_id = INVALID_PAGE_ID);

/* API declaration */
int VtabCreate(sqlite3 *db, void *pAux, int argc, const char *const *argv,
               sqlite3_vtab **ppVtab, char **pzErr);

int VtabConnect(sqlite3 *db, void *pAux, int argc, const char *const *argv,
                sqlite3_vtab **ppVtab, char **pzErr);

int VtabBestIndex(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo);

int VtabDisconnect(sqlite3_vtab *pVtab);

int VtabOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor);

int VtabClose(sqlite3_vtab_cursor *cur);

int VtabFilter(sqlite3_vtab_cursor *pVtabCursor, int idxNum, const char *idxStr,
               int argc, sqlite3_value **argv);

int VtabNext(sqlite3_vtab_cursor *cur);

int VtabEof(sqlite3_vtab_cursor *cur);

int VtabColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i);

int VtabRowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *pRowid);

class VirtualTable {
  friend class Cursor;

public:
  VirtualTable(Schema *schema, BufferPoolManager *buffer_pool_manager,
               Index *index, page_id_t first_page_id = INVALID_PAGE_ID)
      : schema_(schema), index_(index) {
    table_heap_ = new TableHeap(buffer_pool_manager, first_page_id);
  }

  ~VirtualTable() {
    delete schema_;
    delete table_heap_;
    delete index_;
  }

  // insert into table heap
  inline bool InsertTuple(const Tuple &tuple, RID &rid) {
    return table_heap_->InsertTuple(tuple, rid);
  }

  // insert into index
  inline void InsertEntry(const Tuple &tuple, const RID &rid) {
    // construct indexed key tuple
    std::vector<Value> key_values;

    for (auto &i : index_->GetKeyAttrs())
      key_values.push_back(tuple.GetValue(schema_, i));
    Tuple key(key_values, index_->GetKeySchema());
    index_->InsertEntry(key, rid);
  }

  // delete from table heap
  inline bool DeleteTuple(const RID &rid) {
    return table_heap_->DeleteTuple(rid);
  }

  // delete from index
  inline void DeleteEntry(const RID &rid) {
    Tuple deleted_tuple(rid);
    table_heap_->GetTuple(rid, deleted_tuple);
    // construct indexed key tuple
    std::vector<Value> key_values;

    for (auto &i : index_->GetKeyAttrs())
      key_values.push_back(deleted_tuple.GetValue(schema_, i));
    Tuple key(key_values, index_->GetKeySchema());
    index_->DeleteEntry(key);
  }

  // update table heap tuple
  inline bool UpdateTuple(const Tuple &tuple, const RID &rid) {
    // if failed try to delete and insert
    return table_heap_->UpdateTuple(tuple, rid);
  }

  inline TableIterator begin() { return table_heap_->begin(); }

  inline TableIterator end() { return table_heap_->end(); }

  inline Schema *GetSchema() { return schema_; }

  inline Index *GetIndex() { return index_; }

  inline page_id_t GetFirstPageId() { return table_heap_->GetFirstPageId(); }

private:
  sqlite3_vtab base_;
  // virtual table schema
  Schema *schema_;
  // to read/write actual data in table
  TableHeap *table_heap_;
  // to insert/delete index entry
  Index *index_;
};

class Cursor {
public:
  Cursor(VirtualTable *virtual_table)
      : table_iterator_(virtual_table->begin()), virtual_table_(virtual_table) {
  }

  inline void SetScanFlag(bool is_index_scan) {
    is_index_scan_ = is_index_scan;
  }

  inline VirtualTable *GetVirtualTable() { return virtual_table_; }

  inline Schema *GetKeySchema() {
    return virtual_table_->index_->GetKeySchema();
  }
  // return rid at which cursor is currently pointed
  inline int64_t GetCurrentRid() {
    if (is_index_scan_)
      return results[offset_].Get();
    else
      return (*table_iterator_).GetRid().Get();
  }
  // return tuple at which cursor is currently pointed
  inline Tuple GetCurrentTuple() {
    if (is_index_scan_) {
      RID rid = results[offset_];
      Tuple tuple(rid);
      virtual_table_->table_heap_->GetTuple(rid, tuple);
      return tuple;
    } else {
      return *table_iterator_;
    }
  }
  // move cursor up to next
  Cursor &operator++() {
    if (is_index_scan_)
      ++offset_;
    else
      ++table_iterator_;
    return *this;
  }
  // is end of cursor(no more tuple)
  inline bool isEof() {
    if (is_index_scan_)
      return offset_ == static_cast<int>(results.size());
    else
      return table_iterator_ == virtual_table_->end();
  }

  // wrapper around poit scan methods
  inline void ScanKey(const Tuple &key) {
    virtual_table_->index_->ScanKey(key, results);
  }

private:
  sqlite3_vtab_cursor base_; /* Base class - must be first */
  // for index scan
  std::vector<RID> results;
  int offset_ = 0;
  // for sequential scan
  TableIterator table_iterator_;
  // flag to indicate which scan method is currently used
  bool is_index_scan_ = false;
  VirtualTable *virtual_table_;
};

} // namespace cmudb
