/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#include <stdio.h>        // fprintf
#include <getopt.h>       // getopt_long
#include <stdlib.h>       // strtoull

#include "share/ob_define.h"
#include "lib/file/file_directory_utils.h"
#include "ob_log_fetcher_impl.h"

using namespace oceanbase;
using namespace common;
using namespace liboblog;
using namespace fetcher;

#define OB_LOGGER ::oceanbase::common::ObLogger::get_logger()

#define EXPECT_EQ(EXP, VAL) \
  do { \
    if ((EXP) != (VAL)) { _E_("assert failed", #EXP, (EXP), #VAL, (VAL)); exit(1); } \
  } while(0)

namespace oceanbase
{
namespace liboblog
{
namespace integrationtesting
{

/*
 * Add&DiscardTest:
 *  - add n partitions, find m servers for each one, let it dispatch and create new workers
 *  - no log, no heartbeat
 *  - discard all, quit
 */
class AddDiscardTest
{
public:
  int64_t partition_cnt_;
  int64_t server_cnt_;
  int64_t runtime_; // usec
public:
  /*
   * Mock systable helper.
   *  - provide all m servers for each request, 127.0.0.[1-m]
   */
  class MockSystableHelper : public ObILogSysTableHelper
  {
  public:
    int64_t server_cnt_;
    int64_t now_;
    void init(const int64_t svr_cnt)
    {
      server_cnt_ = svr_cnt;
      now_ = get_timestamp();
    }
  public:
    virtual int query_all_clog_history_info_by_log_id_1(
        const common::ObPartitionKey &pkey, const uint64_t log_id,
        AllClogHistoryInfos &records) {
      // Generate random results.
      int ret = OB_SUCCESS;
      records.reset();
      AllClogHistoryInfoRecord rec;
      const int64_t cnt = server_cnt_;
      for (int64_t idx = 0; idx < cnt; ++idx) {
        rec.reset();
        rec.table_id_ = (uint64_t)(pkey.table_id_);
        rec.partition_idx_ = (int32_t)pkey.get_partition_id();
        rec.partition_cnt_ = pkey.get_partition_cnt();//partitoin cnt
        rec.start_log_id_ = log_id;
        rec.end_log_id_ = log_id + 10000;
        rec.start_log_timestamp_ = now_;
        rec.end_log_timestamp_ = now_ + 1 * _HOUR_;
        snprintf(rec.svr_ip_, common::MAX_IP_ADDR_LENGTH + 1, "127.0.0.%ld", 1 + idx);
        rec.svr_port_ = 8888;
        records.push_back(rec);
      }
      return ret;
    }

    virtual int query_all_clog_history_info_by_timestamp_1(
        const common::ObPartitionKey &pkey, const int64_t timestamp,
        AllClogHistoryInfos &records) {
      // Generate random results.
      int ret = OB_SUCCESS;
      records.reset();
      AllClogHistoryInfoRecord rec;
      const int64_t cnt = server_cnt_;
      for (int64_t idx = 0; idx < cnt; ++idx) {
        rec.reset();
        rec.table_id_ = (uint64_t)(pkey.table_id_);
        rec.partition_idx_ = (int32_t)pkey.get_partition_id();
        rec.partition_cnt_ = 0;//partition cnt
        rec.start_log_id_ = 0;
        rec.end_log_id_ = 65536;
        rec.start_log_timestamp_ = timestamp;
        rec.end_log_timestamp_ = timestamp + (1 * _HOUR_);
        snprintf(rec.svr_ip_, common::MAX_IP_ADDR_LENGTH + 1, "127.0.0.%ld", 1 + idx);
        rec.svr_port_ = 8888;
        records.push_back(rec);
      }
      return ret;
    }
    virtual int query_all_meta_table_1(
        const common::ObPartitionKey &pkey, AllMetaTableRecords &records) {
      // Generate random results.
      int ret = OB_SUCCESS;
      UNUSED(pkey);
      records.reset();
      AllMetaTableRecord rec;
      const int64_t cnt = server_cnt_;
      for (int64_t idx = 0; idx < cnt; ++idx) {
        rec.reset();
        snprintf(rec.svr_ip_, common::MAX_IP_ADDR_LENGTH + 1, "127.0.0.%ld", 1 + idx);
        rec.svr_port_ = 8888;
        rec.role_ = (0 == idx) ? LEADER : FOLLOWER;
        records.push_back(rec);
      }
      return ret;
    }
    virtual int query_all_meta_table_for_leader(
        const common::ObPartitionKey &pkey,
        bool &has_leader,
        common::ObAddr &leader)
    {
      UNUSED(pkey);
      has_leader = true;
      leader.set_ip_addr("127.0.0.1", 8888);
      return OB_SUCCESS;
    }
    virtual int query_all_server_table_1(
        AllServerTableRecords &records)
    {
      records.reset();
      AllServerTableRecord rec;
      const int64_t cnt = server_cnt_;
      for (int64_t idx = 0; idx < cnt; ++idx) {
        rec.reset();
        snprintf(rec.svr_ip_, common::MAX_IP_ADDR_LENGTH + 1, "127.0.0.%ld", 1 + idx);
        rec.svr_port_ = 8888;
        records.push_back(rec);
      }
      return OB_SUCCESS;
    }
  };
  /*
   * Rpc.
   *  - return start log id as 1
   *  - no heartbeat
   *  - can open stream
   *  - no log
   */
  class MockRpcInterface : public IFetcherRpcInterface
  {
  private:
    common::ObAddr svr_;

  public:
    ~MockRpcInterface() {}
    virtual void set_svr(const common::ObAddr& svr) { svr_ = svr; }
    virtual const ObAddr& get_svr() const { return svr_; }
    virtual void set_timeout(const int64_t timeout) { UNUSED(timeout); }
    virtual int req_start_log_id_by_ts(
        const obrpc::ObLogReqStartLogIdByTsRequest& req,
        obrpc::ObLogReqStartLogIdByTsResponse& res)
    {
      UNUSED(req);
      UNUSED(res);
      return OB_NOT_IMPLEMENT;
    }
    virtual int req_start_log_id_by_ts_2(const obrpc::ObLogReqStartLogIdByTsRequestWithBreakpoint &req,
                                         obrpc::ObLogReqStartLogIdByTsResponseWithBreakpoint &res) {
      res.reset();
      for (int64_t idx = 0, cnt = req.get_params().count(); idx < cnt; ++idx) {
        obrpc::ObLogReqStartLogIdByTsResponseWithBreakpoint::Result result;
        result.reset();
        result.err_ = OB_SUCCESS;
        result.start_log_id_ = 1;
        res.append_result(result);
      }
      _D_(">>> req start log id", K(req), K(res));
      return OB_SUCCESS;
    }
    virtual int req_start_pos_by_log_id_2(const obrpc::ObLogReqStartPosByLogIdRequestWithBreakpoint &req,
                                          obrpc::ObLogReqStartPosByLogIdResponseWithBreakpoint &res) {
      UNUSED(req);
      UNUSED(res);
      return OB_NOT_IMPLEMENT;
    }
    virtual int req_start_pos_by_log_id(
        const obrpc::ObLogReqStartPosByLogIdRequest& req,
        obrpc::ObLogReqStartPosByLogIdResponse& res)
    {
      UNUSED(req);
      UNUSED(res);
      return OB_NOT_IMPLEMENT;
    }
    virtual int fetch_log(
        const obrpc::ObLogExternalFetchLogRequest& req,
        obrpc::ObLogExternalFetchLogResponse& res)
    {
      UNUSED(req);
      UNUSED(res);
      return OB_NOT_IMPLEMENT;
    }
    virtual int req_heartbeat_info(
        const obrpc::ObLogReqHeartbeatInfoRequest& req,
        obrpc::ObLogReqHeartbeatInfoResponse& res)
    {
      res.reset();
      for (int64_t idx = 0, cnt = req.get_params().count(); idx < cnt; ++idx) {
        obrpc::ObLogReqHeartbeatInfoResponse::Result result;
        result.reset();
        result.err_ = OB_NEED_RETRY;
        result.tstamp_ = OB_INVALID_TIMESTAMP;
        res.append_result(result);
      }
      _D_(">>> heartbeat", K(req), K(res));
      return OB_SUCCESS;
    }

    virtual int req_leader_heartbeat(
        const obrpc::ObLogLeaderHeartbeatReq &req,
        obrpc::ObLogLeaderHeartbeatResp &res)
    {
      res.reset();
      res.set_err(OB_SUCCESS);
      res.set_debug_err(OB_SUCCESS);
      for (int64_t idx = 0, cnt = req.get_params().count(); idx < cnt; ++idx) {
        obrpc::ObLogLeaderHeartbeatResp::Result result;
        const obrpc::ObLogLeaderHeartbeatReq::Param &param = req.get_params().at(idx);

        // table_id为偶数时，认为在访问主observer，返回正常的心跳信息
        // table_id为奇数时，认为在访问备observer，返回停滞的心跳
        bool asking_leader = ((param.pkey_.get_table_id() % 2) == 0);

        result.reset();
        result.err_ = asking_leader ? OB_SUCCESS : OB_NOT_MASTER;
        result.next_served_log_id_ = param.next_log_id_;
        result.next_served_ts_ = asking_leader ? get_timestamp() : 1;

        EXPECT_EQ(OB_SUCCESS, res.append_result(result));
      }

      _D_(">>> heartbeat", K(req), K(res));
      return OB_SUCCESS;
    }

    virtual int open_stream(const obrpc::ObLogOpenStreamReq &req,
                            obrpc::ObLogOpenStreamResp &res) {
      int ret = OB_SUCCESS;
      UNUSED(req);
      obrpc::ObStreamSeq seq;
      seq.reset();
      seq.self_.set_ip_addr("127.0.0.1", 8888);
      seq.seq_ts_ = get_timestamp();
      res.reset();
      res.set_err(OB_SUCCESS);
      res.set_debug_err(OB_SUCCESS);
      res.set_stream_seq(seq);
      _D_(">>> open stream", K(req), K(res));
      return ret;
    }
    virtual int fetch_stream_log(const obrpc::ObLogStreamFetchLogReq &req,
                                 obrpc::ObLogStreamFetchLogResp &res) {
      UNUSED(req);
      res.reset();
      res.set_err(OB_SUCCESS);
      res.set_debug_err(OB_SUCCESS);
      _D_(">>> fetch log", K(req), K(res));
      return OB_SUCCESS;
    }
    virtual int req_svr_feedback(const ReqLogSvrFeedback &feedback)
    {
      UNUSED(feedback);
      return OB_SUCCESS;
    }
  };
  /*
   * Factory.
   */
  class MockRpcInterfaceFactory : public IFetcherRpcInterfaceFactory
  {
  public:
    virtual int new_fetcher_rpc_interface(IFetcherRpcInterface*& rpc)
    {
      rpc = new MockRpcInterface();
      return OB_SUCCESS;
    }
    virtual int delete_fetcher_rpc_interface(IFetcherRpcInterface* rpc)
    {
      delete rpc;
      return OB_SUCCESS;
    }
  };

  /*
   * Mock parser.
   *  - swallow everything
   */
  class MockParser : public IObLogParser
  {
  public:
    MockParser() : trans_cnt_(0) { }
    virtual ~MockParser() { }
    virtual int start() { return OB_SUCCESS; }
    virtual void stop() { }
    virtual void mark_stop_flag() { }
    virtual int push(PartTransTask* task, const int64_t timeout)
    {
      UNUSED(timeout);
      if (NULL != task) {
        task->revert();

        if (task->is_normal_trans()) {
          trans_cnt_ += 1;
        }
      }
      return OB_SUCCESS;
    }
    int64_t get_trans_cnt() const { return trans_cnt_; }
  private:
    int64_t trans_cnt_;
  };
  /*
   * Err handler.
   *  - exit on error
   */
  class MockFetcherErrHandler : public IErrHandler
  {
  public:
    virtual ~MockFetcherErrHandler() { }
  public:
    virtual void handle_err(int err_no, const char* fmt, ...)
    {
      UNUSED(err_no);
      va_list ap;
      va_start(ap, fmt);
      __E__(fmt, ap);
      va_end(ap);
      exit(1);
    }
  };

public:
  void run()
  {
    int err = OB_SUCCESS;

    // Task Pool.
    ObLogTransTaskPool<PartTransTask> task_pool;
    ObConcurrentFIFOAllocator task_pool_alloc;
    err = task_pool_alloc.init(128 * _G_, 8 * _M_, OB_MALLOC_NORMAL_BLOCK_SIZE);
    EXPECT_EQ(OB_SUCCESS, err);
    err = task_pool.init(&task_pool_alloc, 10240, 1024, 4 * 1024 * 1024, true);
    EXPECT_EQ(OB_SUCCESS, err);

    // Parser.
    MockParser parser;

    // Err Handler.
    MockFetcherErrHandler err_handler;

    // Rpc.
    MockRpcInterfaceFactory rpc_factory;

    // Worker Pool.
    FixedJobPerWorkerPool worker_pool;
    err = worker_pool.init(1);
    EXPECT_EQ(OB_SUCCESS, err);

    // StartLogIdLocator.
    ::oceanbase::liboblog::fetcher::StartLogIdLocator locator;
    err = locator.init(&rpc_factory, &err_handler, &worker_pool, 3);
    EXPECT_EQ(OB_SUCCESS, err);

    // Heartbeater.
    Heartbeater heartbeater;
    err = heartbeater.init(&rpc_factory, &err_handler, &worker_pool, 3);
    EXPECT_EQ(OB_SUCCESS, err);

    // SvrFinder.
    MockSystableHelper systable_helper;
    systable_helper.init(server_cnt_);
    ::oceanbase::liboblog::fetcher::SvrFinder svrfinder;
    err = svrfinder.init(&systable_helper, &err_handler, &worker_pool, 3);
    EXPECT_EQ(OB_SUCCESS, err);

    // Fetcher Config.
    FetcherConfig cfg;
    cfg.reset();

    // Init.
    ::oceanbase::liboblog::fetcher::Fetcher fetcher;
    err = fetcher.init(&task_pool, &parser, &err_handler, &rpc_factory,
                       &worker_pool, &svrfinder, &locator, &heartbeater, &cfg);
    EXPECT_EQ(OB_SUCCESS, err);

    // Add partition.
    // partition cnt.need rewrite
    for (int64_t idx = 0, cnt = partition_cnt_; (idx < cnt); ++idx) {
      ObPartitionKey p1(1001 + idx, 0, partition_cnt_);
      err = fetcher.fetch_partition(p1, 1, OB_INVALID_ID);
      EXPECT_EQ(OB_SUCCESS, err);
    }

    // Run.
    err = fetcher.start();
    EXPECT_EQ(OB_SUCCESS, err);

    // Runtime.
    int64_t start = get_timestamp();
    while ((get_timestamp() - start) < runtime_) {
      usec_sleep(500 * _MSEC_);
    }

    // Discard partition.
    // partition cnt.may need rewrite
    for (int64_t idx = 0, cnt = partition_cnt_; (idx < cnt); ++idx) {
      ObPartitionKey p1(1001 + idx, 0, partition_cnt_);
      err = fetcher.discard_partition(p1);
      EXPECT_EQ(OB_SUCCESS, err);
    }

    // Stop.
    err = fetcher.stop(true);
    EXPECT_EQ(OB_SUCCESS, err);

    // Destroy.
    err = fetcher.destroy();
    EXPECT_EQ(OB_SUCCESS, err);
    err = locator.destroy();
    EXPECT_EQ(OB_SUCCESS, err);
    err = svrfinder.destroy();
    EXPECT_EQ(OB_SUCCESS, err);
    err = heartbeater.destroy();
    EXPECT_EQ(OB_SUCCESS, err);
    worker_pool.destroy();
    EXPECT_EQ(OB_SUCCESS, err);
    task_pool.destroy();
    EXPECT_EQ(OB_SUCCESS, err);
  }
};

}
}
}

void print_usage(const char *prog_name)
{
  printf("USAGE: %s\n"
         "   -p, --partition              partition count\n"
         "   -s, --server                 server count\n"
         "   -r, --runtime                run time in seconds, default -1, means to run forever\n",
          prog_name);
}
int main(const int argc, char **argv)
{
  // option variables
  int opt = -1;
  const char *opt_string = "p:s:r:";
  struct option long_opts[] =
      {
          {"partition", 1, NULL, 'p'},
          {"server", 1, NULL, 's'},
          {"runtime", 1, NULL, 'r'},
          {0, 0, 0, 0}
      };

  if (argc <= 1) {
    print_usage(argv[0]);
    return 1;
  }

  // Params.
  int64_t partition_cnt = 0;
  int64_t server_cnt = 0;
  int64_t runtime = 1 * ::oceanbase::liboblog::_YEAR_;

  // Parse command line
  while ((opt = getopt_long(argc, argv, opt_string, long_opts, NULL)) != -1) {
    switch (opt) {
      case 'p': {
        partition_cnt = strtoll(optarg, NULL, 10);
        break;
      }
      case 's': {
        server_cnt = strtoll(optarg, NULL, 10);
        break;
      }
      case 'r': {
        runtime = strtoll(optarg, NULL, 10);
        break;
      }
      default:
        print_usage(argv[0]);
        break;
    } // end switch
  } // end while

  printf("partition_cnt:%ld server_cnt:%ld runtime:%ld sec\n", partition_cnt, server_cnt, runtime);

  // Logger.
  ::oceanbase::liboblog::fetcher::FetcherLogLevelSetter::get_instance().set_mod_log_levels("INFO");
  OB_LOGGER.set_log_level("INFO");
  // Run test.
  ::oceanbase::liboblog::integrationtesting::AddDiscardTest test;
  test.partition_cnt_ = partition_cnt;
  test.server_cnt_ = server_cnt;
  test.runtime_ = ::oceanbase::liboblog::_SEC_ * runtime;
  test.run();
  return 0;
}
