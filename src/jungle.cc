/************************************************************************
Copyright 2017-2019 eBay Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
**************************************************************************/

#include "db_internal.h"
#include "db_mgr.h"
#include "fileops_directio.h"
#include "fileops_posix.h"
#include "flusher.h"
#include "internal_helper.h"
#include "sampler.h"

#include _MACRO_TO_STR(LOGGER_H)

#include <libjungle/jungle.h>

#include <set>

namespace jungle {

const FlushOptions DB::DEFAULT_FLUSH_OPTIONS;

DB::DB() : p(new DBInternal()), sn(nullptr) {}

DB::DB(DB* _parent, uint64_t last_flush, uint64_t checkpoint)
    : p(_parent->p)
    , sn( new SnapInternal(last_flush, checkpoint) )
    {}

DB::~DB() {
    if (!sn) {
        delete p;
    } else {
        delete sn;
    }
}

Status DB::open(DB** ptr_out,
                const std::string& path,
                const DBConfig& db_config)
{
    if (!ptr_out || path.empty() || !db_config.isValid()) {
        return Status::INVALID_PARAMETERS;
    }

    Status s;

    DBMgr* db_mgr = DBMgr::get();
    std::string empty_kvs_name;
    DB* db = db_mgr->openExisting(path, empty_kvs_name);
    if (db) {
        _log_info(db->p->myLog, "Open existing DB %s %p", path.c_str(), db);
        *ptr_out = db;
        return Status();
    }

   try {
    db = new DB();
    db->p->path = path;
    db->p->fOps = new FileOpsPosix();
    db->p->dbConfig = db_config;
    db->p->adjustConfig();

    bool previous_exists = false;
    if ( db->p->fOps->exist(db->p->path) &&
         db->p->fOps->exist(db->p->path + "/db_manifest") ) {
        previous_exists = true;
    }

    if (!previous_exists) {
        if (db->p->dbConfig.readOnly) {
            // Read-only mode: should fail.
            throw Status(Status::FILE_NOT_EXIST);
        }
        // Create the directory.
        db->p->fOps->mkdir(db->p->path.c_str());
    }

    // Start logger if enabled.
    if ( db->p->dbConfig.allowLogging &&
         !db->p->myLog ) {
        std::string logfile = db->p->path + "/system_logs.log";
        db->p->myLog = new SimpleLogger(logfile, 1024, 32*1024*1024, 4);
        db->p->myLog->setLogLevel(4);
        db->p->myLog->setDispLevel(-1);
        db->p->myLog->start();
    }
    db->p->fDirectOps =
        new FileOpsDirectIO(db->p->myLog,
                            db->p->dbConfig.directIoOpt.bufferSize,
                            db->p->dbConfig.directIoOpt.alignSize);
    _log_info(db->p->myLog, "Open new DB handle %s %p", path.c_str(), db);
    _log_info(db->p->myLog, "cache size %zu", db_mgr->getGlobalConfig()->fdbCacheSize);

    if (previous_exists) {
        // DB already exists, load it.
        // Load DB manifest.
        db->p->mani = new DBManifest(db->p->fOps);
        db->p->mani->setLogger(db->p->myLog);
        std::string m_filename = db->p->path + "/db_manifest";
        db->p->mani->load(db->p->path, m_filename);
    } else {
        // Create DB manifest.
        db->p->mani = new DBManifest(db->p->fOps);
        db->p->mani->setLogger(db->p->myLog);
        std::string m_filename = db->p->path + "/db_manifest";
        db->p->mani->create(db->p->path, m_filename);
    }

    // Main DB handle's ID is 0.
    db->p->kvsID = 0;

    // Init log manager.
    // It will manage log-manifest and log files.
    LogMgrOptions log_mgr_opt;
    log_mgr_opt.fOps = db->p->fOps;
    log_mgr_opt.fDirectOps = db->p->fDirectOps;
    log_mgr_opt.path = db->p->path;
    log_mgr_opt.prefixNum = db->p->kvsID;
    log_mgr_opt.dbConfig = &db->p->dbConfig;
    db->p->logMgr = new LogMgr(db);
    db->p->logMgr->setLogger(db->p->myLog);
    TC(db->p->logMgr->init(log_mgr_opt));

    // Init table manager.
    // It will manage table-manifest and table files.
    TableMgrOptions table_mgr_opt;
    table_mgr_opt.fOps = db->p->fOps;
    table_mgr_opt.path = db->p->path;
    table_mgr_opt.prefixNum = db->p->kvsID;
    table_mgr_opt.dbConfig = &db->p->dbConfig;
    db->p->tableMgr = new TableMgr(db);
    db->p->tableMgr->setLogger(db->p->myLog);
    TC(db->p->tableMgr->init(table_mgr_opt));

    // In case of previous crash,
    // sync table's last seqnum if log is lagging behind.
    db->p->logMgr->syncSeqnum(db->p->tableMgr);

    s = db_mgr->assignNew(db);
    if (!s) {
        // Other thread already creates the handle.
        _log_debug(db->p->myLog, "Duplicate DB handle for %s %p", path.c_str(), db);
        db->p->destroy();
        delete db;
        db = db_mgr->openExisting(path, empty_kvs_name);
    }

    *ptr_out = db;
    return Status();

   } catch (Status s) {
    if (db && db->p) db->p->destroy();
    delete db;
    return s;
   }
}

bool DB::isLogSectionMode(const std::string& path) {
    if (!FileMgr::exist(path)) {
        // Path doesn't exist.
        return false;
    }

    // TODO: Other non-default DB as well?
    std::string t_mani_file = path + "/table0000_manifest";
    if (!FileMgr::exist(t_mani_file)) {
        // Table manifest file doesn't exist.
        return false;
    }

    std::unique_ptr<FileOps> f_ops =
        std::unique_ptr<FileOps>( new FileOpsPosix() );

    Status s;
    FileHandle* m_file = nullptr;
    s = f_ops->open(&m_file, t_mani_file);
    if (!s) return false;

    RwSerializer ss(f_ops.get(), m_file);

    // First 8 bytes should be 0xffff...
    uint64_t last_t_num = ss.getU64();

    // Next 8 bytes should be 1.
    uint32_t num_levels = ss.getU32();

    f_ops->close(m_file);
    delete m_file;

    if ( last_t_num == std::numeric_limits<uint64_t>::max() &&
         num_levels == 1 ) return true;

    return false;
}

bool DB::isReadOnly() const {
    return p->dbConfig.readOnly;
}

Status DB::init(const GlobalConfig& global_config) {
    DBMgr* mgr = DBMgr::init(global_config);
    if (!mgr) {
        return Status::ERROR;
    }
    return Status();
}

Status DB::shutdown() {
    DBMgr* mgr = DBMgr::getWithoutInit();
    if (!mgr) return Status::ALREADY_SHUTDOWN;

    bool shutdown_logger = mgr->getGlobalConfig()->shutdownLogger;
    mgr->destroy();

    if (shutdown_logger) {
        SimpleLogger::shutdown();
    }

    return Status();
}

Status DB::close(DB* db) {
    if (db->sn) {
        _log_trace(db->p->myLog, "close snapshot %p", db);
        // This is a snapshot handle.
        db->p->logMgr->closeSnapshot(db);
        db->p->tableMgr->closeSnapshot(db);
        delete db;
        return Status();
    }

    _log_info(db->p->myLog, "close db %p", db);
    if (db->p->dbGroup) {
        // This is a default handle of parent DBGroup handle.
        // Do not close it. It will be closed along with DBGroup handle.
        _log_info(db->p->myLog,
                  "default handle of group %p, defer to close it",
                  db->p->dbGroup);
        return Status();
    }

    DBMgr* mgr = DBMgr::getWithoutInit();
    if (!mgr) return Status::ALREADY_SHUTDOWN;
    return mgr->close(db);
}

Status DB::openSnapshot(DB** snap_out,
                        const uint64_t checkpoint)
{
    Status s;
    EP( p->checkHandleValidity() );

    uint64_t chk_local = checkpoint;
    if (checkpoint) {
        // If checkpoint is given, find exact match.
        std::list<uint64_t> chk_nums;
        EP(getCheckpoints(chk_nums));

        bool found = false;
        for (auto& entry: chk_nums) {
            if (entry == checkpoint) {
                found = true;
                break;
            }
        }
        if (!found) return Status::INVALID_CHECKPOINT;
    } else {
        // If 0, take the latest snapshot.
        // NOTE: Should tolerate error for empty DB.
        p->logMgr->getMaxSeqNum(chk_local);
    }
    uint64_t last_flush_seq = 0;
    p->logMgr->getLastFlushedSeqNum(last_flush_seq);

    DB* snap = new DB(this, last_flush_seq, chk_local);

    p->logMgr->openSnapshot(snap, chk_local, snap->sn->logList);
    // Maybe need same parameter to logList above.
    p->tableMgr->openSnapshot(snap, chk_local, snap->sn->tableList);
    *snap_out = snap;

    return Status();
}

Status DB::rollback(uint64_t seqnum_upto)
{
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_WRITE) );

    // NOTE: Only for log-only mode for now.
    if (!p->dbConfig.logSectionOnly) return Status::INVALID_MODE;

    return p->logMgr->rollback(seqnum_upto);
}

Status DB::set(const KV& kv) {
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_WRITE) );
    return setSN(NOT_INITIALIZED, kv);
}

Status DB::setSN(const uint64_t seq_num, const KV& kv) {
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_WRITE) );

    Record rec;
    rec.kv = kv;
    rec.seqNum = seq_num;
    s = p->logMgr->setSN(rec);
    return s;
}

Status DB::setRecord(const Record& rec) {
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_WRITE) );
    return p->logMgr->setSN(rec);
}

Status DB::setRecordByKey(const Record& rec) {
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_WRITE) );
    Record rec_local = rec;
    rec_local.seqNum = NOT_INITIALIZED;
    return p->logMgr->setSN(rec_local);
}

Status DB::setRecordBatch(const std::list<Record>& batch)
{
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_WRITE) );
    return p->logMgr->setMulti(batch);
}

Status DB::getMaxSeqNum(uint64_t& seq_num_out) {
    Status s;
    EP( p->checkHandleValidity() );
    return p->logMgr->getMaxSeqNum(seq_num_out);
}

Status DB::getMinSeqNum(uint64_t& seq_num_out) {
    Status s;
    EP( p->checkHandleValidity() );
    return p->logMgr->getMinSeqNum(seq_num_out);
}

Status DB::getLastFlushedSeqNum(uint64_t& seq_num_out) {
    Status s;
    EP( p->checkHandleValidity() );
    return p->logMgr->getLastFlushedSeqNum(seq_num_out);
}

Status DB::getLastSyncedSeqNum(uint64_t& seq_num_out) {
    Status s;
    EP( p->checkHandleValidity() );
    return p->logMgr->getLastSyncedSeqNum(seq_num_out);
}

Status DB::getCheckpoints(std::list<uint64_t>& chk_out) {
    Status s;
    EP( p->checkHandleValidity() );

    std::list<uint64_t> chk_local;
    EP(p->logMgr->getAvailCheckpoints(chk_local));
    if (chk_local.size() < p->dbConfig.maxKeepingCheckpoints) {
        // Only when the number of checkpoints is less than the limit.
        EP(p->tableMgr->getAvailCheckpoints(chk_local));
    }

    // Asc order sort, remove duplicates.
    std::set<uint64_t> chk_sorted;
    for (auto& entry: chk_local) chk_sorted.insert(entry);

    // Only pick last N checkpoints.
    chk_out.clear();
    auto entry = chk_sorted.rbegin();
    while (entry != chk_sorted.rend()) {
        if (chk_out.size() >= p->dbConfig.maxKeepingCheckpoints) break;
        chk_out.push_front(*entry);
        entry++;
    }
    return Status();
}

Status DB::sync(bool call_fsync) {
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_FLUSH) );
    s = p->logMgr->sync(call_fsync);
    return s;
}

Status DB::syncNoWait(bool call_fsync) {
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_FLUSH) );
    s = p->logMgr->syncNoWait(call_fsync);
    return s;
}

Status DB::discardDirty(uint64_t seq_num_begin) {
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_FLUSH) );
    s = p->logMgr->discardDirty(seq_num_begin);
    return s;
}

Status DB::flushLogs(const FlushOptions& options, const uint64_t seq_num) {
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_FLUSH) );

    FlushOptions local_options = options;
    if (p->dbConfig.logSectionOnly) local_options.purgeOnly = true;

    _log_info(p->myLog, "Flush logs, upto %s (purgeOnly = %s).",
              _seq_str(seq_num).c_str(), (local_options.purgeOnly)?"true":"false");

    s = p->logMgr->flush(local_options, seq_num, p->tableMgr);
    return s;
}

Status DB::flushLogsAsync(const FlushOptions& options,
                          UserHandler handler,
                          void* ctx,
                          const uint64_t seq_num)
{
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_FLUSH) );

    DBMgr* db_mgr = DBMgr::getWithoutInit();
    if (!db_mgr) return Status::NOT_INITIALIZED;

    FlushOptions local_options = options;
    if (p->dbConfig.logSectionOnly) local_options.purgeOnly = true;

    // Selective logging based on timer, to avoid verbose messages.
    int num_suppressed = 0;
    SimpleLogger::Levels lv = p->vlAsyncFlush.checkPrint(num_suppressed)
                              ? SimpleLogger::INFO
                              : SimpleLogger::DEBUG;
    num_suppressed = (p->myLog && p->myLog->getLogLevel() >= SimpleLogger::DEBUG)
                     ? 0 : num_suppressed;

    _log_( lv, p->myLog,
           "Request async log flushing, upto %s "
           "(purgeOnly = %s, syncOnly = %s, callFsync = %s, delay = %zu), "
           "%d messages suppressed",
           _seq_str(seq_num).c_str(),
           (local_options.purgeOnly) ? "true" : "false",
           (local_options.syncOnly)  ? "true" : "false",
           (local_options.callFsync) ? "true" : "false",
           local_options.execDelayUs,
           num_suppressed );

    FlusherQueueElem* elem =
        new FlusherQueueElem(this, local_options, seq_num, handler, ctx);
    db_mgr->flusherQueue()->push(elem);

    if (options.execDelayUs) {
        // Delay is given.
        std::lock_guard<std::mutex> l(p->asyncFlushJobLock);
        if ( !p->asyncFlushJob ||
             p->asyncFlushJob->isDone() ) {
            // Schedule a new timer.
            p->asyncFlushJob = db_mgr->getTpMgr()->addTask(
                [db_mgr, lv, this](const simple_thread_pool::TaskResult& ret) {
                    if (!ret.ok()) return;
                    _log_(lv, p->myLog, "delayed flushing wakes up");
                    db_mgr->workerMgr()->invokeWorker("flusher");
                },
                local_options.execDelayUs );
            _log_(lv, p->myLog, "scheduled delayed flushing %p, %zu us",
                  p->asyncFlushJob.get(),
                  local_options.execDelayUs );
        }
    } else {
        // Immediately invoke.
        _log_(lv, p->myLog, "invoke flush worker");
        db_mgr->workerMgr()->invokeWorker("flusher");
    }

    return Status();
}

Status DB::checkpoint(uint64_t& seq_num_out, bool call_fsync) {
    Status s;
    s = p->logMgr->checkpoint(seq_num_out, call_fsync);
    if (s) {
        _log_info(p->myLog, "Added new checkpoint for %ld.", seq_num_out);
    } else {
        _log_err(p->myLog, "checkpoint returned %d.", s);
    }
    return s;
}

Status DB::compactL0(const CompactOptions& options,
                     uint32_t hash_num) {
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_COMPACT) );
    s = p->tableMgr->compactL0(options, hash_num);
    return s;
}

Status DB::compactLevel(const CompactOptions& options,
                        size_t level)
{
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_COMPACT) );
    if (p->dbConfig.nextLevelExtension) {
        if (level == 0) {
            // In level extension mode, level-0 is based on
            // hash partition. We should call `compactL0()`.
            return Status::INVALID_LEVEL;
        }
        s = p->tableMgr->compactLevelItr(options, nullptr, level);
    }
    return s;
}

Status DB::compactInplace(const CompactOptions& options,
                          size_t level)
{
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_COMPACT) );
    if (level == 0) return Status::INVALID_LEVEL;
    s = p->tableMgr->compactInPlace(options, nullptr, level, options.ignoreThreshold);
    return s;
}
Status DB::compactIdxUpto(const CompactOptions& options,
                          size_t idx_upto,
                          size_t expiry_sec)
{
    p->tableMgr->setUrgentCompactionTableIdx(idx_upto, expiry_sec);
    return Status();
}

Status DB::splitLevel(const CompactOptions& options,
                      size_t level)
{
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_COMPACT) );
    if (level == 0) return Status::INVALID_LEVEL;
    s = p->tableMgr->splitLevel(options, nullptr, level);
    return s;
}

Status DB::mergeLevel(const CompactOptions& options,
                      size_t level)
{
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_COMPACT) );
    if (level == 0) return Status::INVALID_LEVEL;
    s = p->tableMgr->mergeLevel(options, nullptr, level);
    return s;
}


// NOTE:
//  MemTable --> LogFile --> LogMgr --(memcpy)--+--> DB ---> User
//  TableFile --(memcpy)---> TableMgr ----------+

Status DB::get(const SizedBuf& key, SizedBuf& value_out) {
    Status s;
    EP( p->checkHandleValidity() );

    Record rec_log;
    Record::Holder h_rec_log(rec_log);
    uint64_t chknum = (sn)?(sn->chkNum):(NOT_INITIALIZED);
    std::list<LogFileInfo*>* l_list = (sn)?(sn->logList):(nullptr);
    s = p->logMgr->get(chknum, l_list, key, rec_log);
    if (s) {
        if (!rec_log.isIns()) {
            // Removed key, should return without searching tables.
            return Status::KEY_NOT_FOUND;
        }
        rec_log.kv.value.moveTo(value_out);
        return s;
    }

    // Not exist in log, but maybe they have been purged.
    // Search in table.
    Record rec;
    Record::Holder h_rec(rec);

    // Temporarily assign memory given by user, SHOULD NOT BE FREED.
    rec.kv.key = key;
    DB* snap_handle = (this->sn)?(this):(nullptr);
    s = p->tableMgr->get(snap_handle, rec);
    // Clear the memory by user, to avoid freeing it.
    rec.kv.key.clear();
    if (s) {
        if (!rec.isIns()) {
            // Removed key.
            return Status::KEY_NOT_FOUND;
        }
        rec.kv.value.moveTo(value_out);
    }
    return s;
}

Status DB::getSN(const uint64_t seq_num, KV& kv_out) {
    Status s;
    EP( p->checkHandleValidity() );

    Record rec;
    Record::Holder h_rec(rec);
    s = p->logMgr->getSN(seq_num, rec);
    if (!s) return s;
    if (!rec.isIns()) {
        return Status::NOT_KV_PAIR;
    }
    rec.kv.moveTo(kv_out);
    return s;
}

Status DB::getRecord(const uint64_t seq_num, Record& rec_out) {
    Status s;
    EP( p->checkHandleValidity() );

    Record rec;
    Record::Holder h_rec(rec);
    s = p->logMgr->getSN(seq_num, rec);
    if (!s) return s;
    rec.moveTo(rec_out);
    return s;
}

Status DB::getRecordByKey(const SizedBuf& key,
                          Record& rec_out,
                          bool meta_only)
{
    Status s;
    EP( p->checkHandleValidity() );

    Record rec_log;
    Record::Holder h_rec_log(rec_log);
    uint64_t chknum = (sn)?(sn->chkNum):(NOT_INITIALIZED);
    std::list<LogFileInfo*>* l_list = (sn)?(sn->logList):(nullptr);
    s = p->logMgr->get(chknum, l_list, key, rec_log);
    if (s) {
        if (!meta_only && !rec_log.isIns()) {
            // NOT meta only mode + removed key:
            // should return without searching tables.
            return Status::KEY_NOT_FOUND;
        }
        if (meta_only) {
            rec_log.kv.value.free();
        }
        rec_log.moveTo(rec_out);
        return s;
    }

    // Not exist in log, but maybe they have been purged.
    // Search in table.
    key.copyTo(rec_out.kv.key);
    DB* snap_handle = (this->sn)?(this):(nullptr);
    s = p->tableMgr->get(snap_handle, rec_out, meta_only);
    if (s) {
        if (!meta_only && !rec_out.isIns()) {
            // Removed key, return false if not `meta_only` mode.
            rec_out.free();
            return Status::KEY_NOT_FOUND;
        }
    } else {
        rec_out.free();
    }
    return s;
}

Status DB::getNearestRecordByKey(const SizedBuf& key,
                                 Record& rec_out,
                                 SearchOptions opt,
                                 bool meta_only)
{
    Status s;
    EP( p->checkHandleValidity() );

    if ( opt != SearchOptions::GREATER_OR_EQUAL &&
         opt != SearchOptions::GREATER &&
         opt != SearchOptions::SMALLER_OR_EQUAL &&
         opt != SearchOptions::SMALLER ) {
        return Status::INVALID_PARAMETERS;
    }

    Record rec_from_log;
    Record::Holder h_rec_from_log(rec_from_log);
    uint64_t chknum = (sn)?(sn->chkNum):(NOT_INITIALIZED);
    std::list<LogFileInfo*>* l_list = (sn)?(sn->logList):(nullptr);
    p->logMgr->getNearest(chknum, l_list, key, rec_from_log, opt);

    Record rec_from_table;
    Record::Holder h_rec_from_table(rec_from_table);
    DB* snap_handle = (this->sn)?(this):(nullptr);
    p->tableMgr->getNearest(snap_handle, key, rec_from_table, opt, meta_only);

    if (rec_from_log.empty() && rec_from_table.empty()) {
        // Not found from both.
        return Status::KEY_NOT_FOUND;
    }

    // Return the closer one. If the same, return the newer
    // (higher seq number) one.
    int cmp = 0;
    bool choose_log = true;
    if (!rec_from_log.empty() && !rec_from_table.empty()) {
        // WARNING:
        //   Comparison is valid only when both are not empty.
        if (p->dbConfig.cmpFunc) {
            cmp = p->dbConfig.cmpFunc( rec_from_log.kv.key.data,
                                       rec_from_log.kv.key.size,
                                       rec_from_table.kv.key.data,
                                       rec_from_table.kv.key.size,
                                       p->dbConfig.cmpFuncParam );
        } else {
            cmp = SizedBuf::cmp(rec_from_log.kv.key, rec_from_table.kv.key);
        }
        if (cmp == 0) {
            // Keys from log and table are the same. Compare the sequence numbers.
            choose_log = (rec_from_log.seqNum >= rec_from_table.seqNum);
        } else {
            if (opt.isGreater()) {
                // Greater: choose smaller one.
                // e.g.)
                //   find greater than `key01`.
                //   * log:   `key01` exists.
                //   * table: `key02` is the smallest key greater than `key01`.
                //   then we should choose log (smaller one).
                choose_log = (cmp < 0);
            } else if (opt.isSmaller()) {
                // Smaller: choose greater one.
                // e.g.)
                //   find smaller than `key01`.
                //   * log:   `key01` exists.
                //   * table: `key00` is the greatest key smaller than `key01`.
                //   then we should choose log (greater one).
                choose_log = (cmp > 0);
            }
        }
    } else {
        if (rec_from_log.empty()) choose_log = false;
        if (rec_from_table.empty()) choose_log = true;
    }

    if (choose_log) {
        // Choose the one from log.
        rec_from_log.moveTo(rec_out);
    } else {
        // Choose the one from table.
        rec_from_table.moveTo(rec_out);
    }
    return s;
}

Status DB::getRecordsByPrefix(const SizedBuf& prefix,
                              SearchCbFunc cb_func)
{
    Status s;
    EP( p->checkHandleValidity() );

    uint64_t record_count = 0;
    SearchCbFunc cb_wrapper = [&record_count, &cb_func]
                              (const SearchCbParams& params) -> SearchCbDecision {
        record_count++;
        return cb_func(params);
    };

    Record rec_local;
    uint64_t chknum = (sn)?(sn->chkNum):(NOT_INITIALIZED);
    std::list<LogFileInfo*>* l_list = (sn)?(sn->logList):(nullptr);
    s = p->logMgr->getPrefix(chknum, l_list, prefix, cb_wrapper);
    if (s == Status::OPERATION_STOPPED) {
        // User wants to stop, return OK.
        return Status::OK;
    } else if ( !s.ok() && s != Status::KEY_NOT_FOUND ) {
        // Stopped by other reason.
        return s;
    }

    // Key not found or user wants to continue, search tables.
    DB* snap_handle = (this->sn)?(this):(nullptr);
    s = p->tableMgr->getPrefix(snap_handle, prefix, cb_wrapper);
    if ( !s.ok() &&
         s != Status::OPERATION_STOPPED &&
         s != Status::KEY_NOT_FOUND ) {
        // Stopped by other reason.
        // NOTE:
        //   `KEY_NOT_FOUND` should be tolerable, as there might be matching
        //   records in the log section.
        return s;
    }
    if (!record_count) {
        return Status::KEY_NOT_FOUND;
    }
    return Status::OK;
}

Status DB::del(const SizedBuf& key) {
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_WRITE) );
    return delSN(NOT_INITIALIZED, key);
}

Status DB::delSN(const uint64_t seq_num, const SizedBuf& key) {
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_WRITE) );

    // Add a deletion marker for the given key.
    Record rec;
    rec.kv.key = key;
    rec.seqNum = seq_num;
    rec.type = Record::Type::DELETION;
    s = p->logMgr->setSN(rec);
    return s;
}

Status DB::delRecord(const Record& rec) {
    Status s;
    EP( p->checkHandleValidity(DBInternal::OPTYPE_WRITE) );

    Record rec_local = rec;
    rec_local.type = Record::DELETION;
    return p->logMgr->setSN(rec_local);
}

Status DB::getStats(DBStats& stats_out, const DBStatsOptions& opt) {
    Status s;
    EP( p->checkHandleValidity() );

    DBMgr* mgr = DBMgr::getWithoutInit();
    if (!mgr) return Status::NOT_INITIALIZED;

    stats_out.cacheSizeByte = mgr->getGlobalConfig()->fdbCacheSize;

    // Number of entries in log.
    uint64_t num_kvs_log = 0;
    if (p) {
        Status s;
        uint64_t min_seq = 0, max_seq = 0;
        s = p->logMgr->getAvailSeqRange(min_seq, max_seq);
        if ( s &&
             valid_number(min_seq) &&
             valid_number(max_seq) &&
             max_seq >= min_seq ) {
            if (min_seq) {
                num_kvs_log = max_seq - min_seq + 1;
            } else {
                // Flush never happened.
                num_kvs_log = max_seq;
            }
        }
    }

    if (p) {
        if (p->logMgr) {
            stats_out.numOpenMemtables = p->logMgr->getNumMemtables();
            stats_out.minLogIndex = p->logMgr->getMinLogFileIndex();
            stats_out.maxLogIndex = p->logMgr->getMaxLogFileIndex();
        }
        stats_out.numBgTasks = p->getNumBgTasks();
    }

    TableStats t_stats;
    if (p && p->tableMgr) {
        p->tableMgr->getStats( t_stats,
                               ( opt.getTableHierarchy
                                 ? &stats_out.tableHierarchy
                                 : nullptr ) );
    }
    stats_out.numKvs = t_stats.numKvs + num_kvs_log;
    stats_out.workingSetSizeByte = t_stats.workingSetSizeByte;
    stats_out.numIndexNodes = t_stats.numIndexNodes;
    stats_out.cacheUsedByte = t_stats.cacheUsedByte;
    stats_out.minTableIndex = t_stats.minTableIdx;
    stats_out.maxTableIndex = t_stats.maxTableIdx;

    return Status();
}

Status DB::getSampleKeys(const SamplingParams& params, std::list<SizedBuf> &keys_out) {
    return Sampler::getSampleKeys(this, params, keys_out);
}

void DB::setLogLevel(int new_level) {
    p->myLog->setLogLevel(new_level);
}

int DB::getLogLevel() const {
    return p->myLog->getLogLevel();
}

std::string DB::getPath() const {
    return p->path;
}

void DB::setDebugParams(const DebugParams& to,
                        size_t effective_time_sec)
{
    DBMgr* mgr = DBMgr::getWithoutInit();
    if (!mgr) return;
    mgr->setDebugParams(to, effective_time_sec);
}

DebugParams DB::getDebugParams() {
    static DebugParams default_debug_params;
    DBMgr* mgr = DBMgr::getWithoutInit();
    if (!mgr) return default_debug_params;
    return mgr->getDebugParams();
}

bool DB::enableDebugCallbacks(bool enable) {
    DBMgr* mgr = DBMgr::getWithoutInit();
    if (!mgr) return false;
    return mgr->enableDebugCallbacks(enable);
}

// === Internal ================================================================

DB::DBInternal::DBInternal()
    : dbGroup(nullptr)
    , wrapper(nullptr)
    , fOps(nullptr)
    , fDirectOps(nullptr)
    , mani(nullptr)
    , kvsID(0)
    , logMgr(nullptr)
    , tableMgr(nullptr)
    , myLog(nullptr)
    , vlAsyncFlush(VERBOSE_LOG_SUPPRESS_MS)
    , asyncFlushJob(nullptr)
{}

DB::DBInternal::~DBInternal() {}

void DB::DBInternal::destroy() {
    flags.closing = true;
    if (tableMgr) tableMgr->disallowCompaction();
    waitForBgTasks();

    {   // Cancel async flush if exists.
        std::lock_guard<std::mutex> l(asyncFlushJobLock);
        if (asyncFlushJob) {
            bool ret = asyncFlushJob->cancel();
            _log_info(myLog, "cancel delayed async flush job %p: %d",
                      asyncFlushJob.get(), ret);
        }
    }

    if (!kvsID) { // Only default DB.
        if (!dbConfig.readOnly) {
            mani->store(true);
        }
        DELETE(mani);
    }

    if (logMgr) logMgr->close();
    if (tableMgr) tableMgr->close();
    DELETE(logMgr);
    DELETE(tableMgr);

    // WARNING:
    //   fOps MUST be freed after both log & table manager, as
    //   the destructor of both managers use file operations for
    //   closing their files.
    DELETE(fOps);
    DELETE(fDirectOps);
    if (!kvsID) { // Only default DB.
        DELETE(myLog);
    }
}

void DB::DBInternal::adjustConfig() {
    // At least one checkpoint should be allowed.
    if (dbConfig.maxKeepingCheckpoints == 0) {
        dbConfig.maxKeepingCheckpoints = 1;
    }
}

void DB::DBInternal::waitForBgTasks() {
    _log_info(myLog, "%zu background tasks are in progress",
              flags.onGoingBgTasks.load());
    uint64_t ticks = 0;
    while (flags.onGoingBgTasks && ticks < DB::DBInternal::MAX_CLOSE_RETRY) {
        ticks++;
        Timer::sleepMs(100);
    }
    _log_info(myLog, "%zu background tasks are in progress, %zu ticks",
              flags.onGoingBgTasks.load(), ticks);
}

void DB::DBInternal::updateOpHistory(size_t amount) {
    DBMgr* mgr = DBMgr::getWithoutInit();
    if (!mgr) return;
    mgr->updateOpHistory(amount);
}

Status DB::DBInternal::checkHandleValidity(OpType op_type) {
    if (flags.closing) return Status::HANDLE_IS_BEING_CLOSED;
    if ( op_type != DBInternal::OPTYPE_READ &&
         flags.rollbackInProgress ) {
        _log_warn(myLog, "attempt to mutate db while rollback is in progress, "
                  "op type %d",
                  op_type);
        return Status::ROLLBACK_IN_PROGRESS;
    }
    return Status::OK;
}

} // namespace jungle

