// Copyright © 2014 Wei Wang. All Rights Reserved.
// 2014-06-28 14:41

/**
 * This file is the main entrance of the program.
 * User can register their own defined  classes, e.g., layers
 */

#include <gflags/gflags.h>
#include <glog/logging.h>
#include "utils/global_context.h"
#include "utils/proto_helper.h"
#include "proto/model.pb.h"
#include "proto/system.pb.h"
#include "coordinator.h"
#include "datasource/data_loader.h"
#include "worker.h"
#include "da/gary.h"


DEFINE_string(system_conf, "examples/imagenet12/system.conf", "configuration file for node roles");
DEFINE_string(model_conf, "examples/imagenet12/model.conf", "DL model configuration file");
DEFINE_bool(load, false, "Load data to distributed tables");
DEFINE_bool(run, true,  "Run training algorithm");
DEFINE_bool(time, true,  "time training algorithm");
// for debugging use
#ifndef FLAGS_v
  DEFINE_int32(v, 3, "vlog controller");
#endif

// for debugging use
int main(int argc, char **argv) {
  int provided;
  MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
  //FLAGS_stderrthreshold=0;
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  LOG(ERROR)<<"mpi thread level provided: "<<provided;
  LOG(ERROR)<<"load data "<<FLAGS_load<<" run "<<FLAGS_run;

  // Note you can register you own layer/edge/datasource here

  // Init GlobalContext
  auto gc=lapis::GlobalContext::Get(FLAGS_system_conf);
  LOG(ERROR)<<"group id"<<gc->group_id();
  lapis::ModelProto model;
  lapis::ReadProtoFromTextFile(FLAGS_model_conf.c_str(), &model);
  if(FLAGS_load) {
    LOG(ERROR)<<"Loading Data...";
    lapis::DataLoader loader(gc);
    if(gc->AmICoordinator())
      loader.ShardData(model.data());
    else if(gc->AmIWorker())
      loader.CreateLocalShards(model.data());

    LOG(ERROR)<<"Finish Load Data";
  }
  if(FLAGS_run){
    lapis::GAry::Init(gc->rank(), gc->groups());
    if(gc->AmICoordinator()) {
      lapis::Coordinator coordinator(gc);
      coordinator.Start(model);
    }else {
      // worker or table server
      lapis::Worker worker(gc);
      worker.Start(model.data(), model.solver());
    }
    lapis::GAry::Finalize();
  }
  /*
  if(FLAGS_resume){
     if(gc->AmICoordinator()) {
      lapis::Coordinator coordinator(gc);
      coordinator.Resume();
    }else {
      lapis::Worker worker(gc);
      worker.Resume(model.data());
    }
  }
  */
  gc->Finish();
  return 0;
}
