/*
Copyright (c) 2021, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License, version 2.0,
as published by the Free Software Foundation.

This program is also distributed with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have included with MySQL.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>

#ifdef RAPIDJSON_NO_SIZETYPEDEFINE
// if we build within the server, it will set RAPIDJSON_NO_SIZETYPEDEFINE
// globally and require to include my_rapidjson_size_t.h
#include "my_rapidjson_size_t.h"
#endif

#include <gmock/gmock.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/schema.h>
#include <rapidjson/stringbuffer.h>

#include "keyring/keyring_manager.h"
#include "mock_server_rest_client.h"
#include "mock_server_testutils.h"
#include "mysql_session.h"
#include "mysqlrouter/cluster_metadata.h"
#include "mysqlrouter/rest_client.h"
#include "mysqlrouter/utils.h"
#include "router_component_clusterset.h"
#include "router_component_test.h"
#include "router_component_testutils.h"
#include "tcp_port_pool.h"

using mysqlrouter::ClusterType;
using mysqlrouter::MySQLSession;
using ::testing::PrintToString;
using namespace std::chrono_literals;

Path g_origin_path;

class ClusterSetTest : public RouterComponentClusterSetTest {
 protected:
  void SetUp() override { RouterComponentTest::SetUp(); }

  std::string get_metadata_cache_section(
      const std::chrono::milliseconds ttl = kTTL,
      const std::string &cluster_type_str = "clusterset") {
    auto ttl_str = std::to_string(std::chrono::duration<double>(ttl).count());

    return "[metadata_cache:test]\n"
           "cluster_type=" +
           cluster_type_str +
           "\n"
           "router_id=1\n" +
           "user=mysql_router1_user\n"
           "metadata_cluster=test\n"
           "connect_timeout=1\n"
           "ttl=" +
           ttl_str + "\n\n";
  }

  std::string get_metadata_cache_routing_section(uint16_t router_port,
                                                 const std::string &role,
                                                 const std::string &strategy) {
    std::string result =
        "[routing:test_default" + std::to_string(router_port) +
        "]\n"
        "bind_port=" +
        std::to_string(router_port) + "\n" +
        "destinations=metadata-cache://test/default?role=" + role +
        /*disconnect_rules +*/ "\n" + "protocol=classic\n";

    if (!strategy.empty())
      result += std::string("routing_strategy=" + strategy + "\n");

    return result;
  }

  auto &launch_router(int expected_errorcode = EXIT_SUCCESS,
                      std::chrono::milliseconds wait_for_notify_ready = 10s) {
    SCOPED_TRACE("// Prepare the dynamic state file for the Router");
    const auto clusterset_all_nodes_ports =
        clusterset_data_.get_all_nodes_classic_ports();
    router_state_file =
        create_state_file(temp_test_dir.name(),
                          create_state_file_content("", clusterset_data_.uuid,
                                                    clusterset_all_nodes_ports,
                                                    /*view_id*/ 1));

    SCOPED_TRACE("// Prepare the config file for the Router");
    const std::string metadata_cache_section = get_metadata_cache_section(kTTL);
    router_port_rw = port_pool_.get_next_available();
    router_port_ro = port_pool_.get_next_available();
    const std::string routing_section_rw = get_metadata_cache_routing_section(
        router_port_rw, "PRIMARY", "first-available");
    const std::string routing_section_ro = get_metadata_cache_routing_section(
        router_port_ro, "SECONDARY", "round-robin");
    const std::string masterkey_file =
        Path(temp_test_dir.name()).join("master.key").str();
    const std::string keyring_file =
        Path(temp_test_dir.name()).join("keyring").str();
    mysql_harness::init_keyring(keyring_file, masterkey_file, true);
    mysql_harness::Keyring *keyring = mysql_harness::get_keyring();
    keyring->store("mysql_router1_user", "password", "root");
    mysql_harness::flush_keyring();
    mysql_harness::reset_keyring();

    // launch the router with metadata-cache configuration
    auto default_section = get_DEFAULT_defaults();
    default_section["keyring_path"] = keyring_file;
    default_section["master_key_path"] = masterkey_file;
    default_section["dynamic_state"] = router_state_file;
    router_conf_file = create_config_file(
        temp_test_dir.name(),
        metadata_cache_section + routing_section_rw + routing_section_ro,
        &default_section);
    auto &router = ProcessManager::launch_router(
        {"-c", router_conf_file}, expected_errorcode, /*catch_stderr=*/true,
        /*with_sudo=*/false, wait_for_notify_ready);
    return router;
  }

  auto &relaunch_router(const std::string &conf_file,
                        int expected_errorcode = EXIT_SUCCESS,
                        std::chrono::milliseconds wait_for_notify_ready = 10s) {
    auto &router = ProcessManager::launch_router(
        {"-c", conf_file}, expected_errorcode, /*catch_stderr=*/true,
        /*with_sudo=*/false, wait_for_notify_ready);
    return router;
  }

  int get_int_global_value(const uint16_t http_port, const std::string &name) {
    const auto server_globals =
        MockServerRestClient(http_port).get_globals_as_json_string();

    return get_int_field_value(server_globals, name);
  }

  // @brief wait until global read from the mock server is greater or equal
  // epxected threashold
  // @retval true selected global is greated or equal expectred threashold
  // @retval false timed out waiting for selectged global to become greater or
  // equal expected threashold
  bool wait_global_ge(const uint16_t http_port, const std::string &name,
                      int threashold, std::chrono::milliseconds timeout = 15s) {
    const auto kStep = 100ms;
    do {
      const auto value = get_int_global_value(http_port, name);
      if (value >= threashold) return true;
      std::this_thread::sleep_for(kStep);
      timeout -= kStep;
    } while (timeout >= 0ms);

    return false;
  }

  void verify_only_primary_gets_updates(const unsigned primary_cluster_id,
                                        const unsigned primary_node_id = 0) {
    // <cluster_id, node_id>
    using NodeId = std::pair<unsigned, unsigned>;
    std::map<NodeId, size_t> count;

    // in the first run pick up how many times the last_check_in updte was
    // performed on each node so far
    for (const auto &cluster : clusterset_data_.clusters) {
      unsigned node_id = 0;
      for (const auto &node : cluster.nodes) {
        count[NodeId(cluster.id, node_id)] =
            get_int_global_value(node.http_port, "update_last_check_in_count");
        ++node_id;
      }
    }

    // in the next step wait for the counter to be incremented on the primary
    // node
    const auto http_port = clusterset_data_.clusters[primary_cluster_id]
                               .nodes[primary_node_id]
                               .http_port;
    EXPECT_TRUE(
        wait_global_ge(http_port, "update_last_check_in_count",
                       count[NodeId(primary_cluster_id, primary_node_id)] + 1));

    // the counter for all other nodes should not change
    for (const auto &cluster : clusterset_data_.clusters) {
      unsigned node_id = 0;
      for (const auto &node : cluster.nodes) {
        // only primary node of the primary cluster is expected do the
        // metadata version update and last_check_in updates
        if (cluster.id != primary_cluster_id || node_id != primary_node_id) {
          EXPECT_EQ(get_int_global_value(node.http_port,
                                         "update_last_check_in_count"),
                    count[NodeId(cluster.id, node_id)]);
        }
        ++node_id;
      }
    }
  }

  int get_update_version_count(const std::string &json_string) {
    return get_int_field_value(json_string, "update_version_count");
  }

  int get_update_last_check_in_count(const std::string &json_string) {
    return get_int_field_value(json_string, "update_last_check_in_count");
  }

  std::string router_conf_file;

  TempDirectory temp_test_dir;
  uint64_t view_id = 1;

  std::string router_state_file;
  uint16_t router_port_rw;
  uint16_t router_port_ro;

  static const std::chrono::milliseconds kTTL;
  static const unsigned kRWNodeId = 0;
  static const unsigned kRONodeId = 1;

  const unsigned kPrimaryClusterId{0};
  const unsigned kFirstReplicaClusterId{1};
  const unsigned kSecondReplicaClusterId{2};
};

const std::chrono::milliseconds ClusterSetTest::kTTL = 50ms;

//////////////////////////////////////////////////////////////////////////

struct TargetClusterTestParams {
  // target_cluster= for the config file
  std::string target_cluster;
  // id of the target Cluster within ClusterSet
  unsigned target_cluster_id;

  // which cluster we expect to handle the connections (same for RW and RO)
  unsigned expected_connection_cluster_id{99};

  std::string expected_error{""};
};

class ClusterSetTargetClusterTest
    : public ClusterSetTest,
      public ::testing::WithParamInterface<TargetClusterTestParams> {};

/**
 * @test Checks that the target cluster from the configuration file is respected
 * and the Router is using expected cluster for client RW and RO connections.
 * [@FR3.6]
 */
TEST_P(ClusterSetTargetClusterTest, ClusterSetTargetCluster) {
  const auto target_cluster = GetParam().target_cluster;
  const auto target_cluster_id = GetParam().target_cluster_id;
  const auto expected_connection_cluster_id =
      GetParam().expected_connection_cluster_id;

  create_clusterset(
      view_id, target_cluster_id, /*primary_cluster_id*/ 0,
      "metadata_clusterset.js",
      /*router_options*/ R"({"targetCluster" : ")" + target_cluster + "\" }");

  SCOPED_TRACE("// Launch the Router");
  /*auto &router =*/launch_router();

  SCOPED_TRACE(
      "// Make the connections to both RW and RO ports and check if they are "
      "directed to expected Cluster from the ClusterSet");

  if (target_cluster_id == 0 /*primary_cluster_id*/) {
    make_new_connection_ok(
        router_port_rw,
        clusterset_data_.clusters[expected_connection_cluster_id]
            .nodes[kRWNodeId]
            .classic_port);
  } else {
    /* replica cluster*/
    verify_new_connection_fails(router_port_rw);
  }

  //   in case of replica cluster first RO node is primary node of the Cluster
  const auto first_ro_node = (target_cluster_id == 0) ? kRONodeId : kRWNodeId;

  make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[expected_connection_cluster_id]
                          .nodes[first_ro_node]
                          .classic_port);
}

INSTANTIATE_TEST_SUITE_P(
    ClusterSetTargetCluster, ClusterSetTargetClusterTest,
    ::testing::Values(
        // 0) we use "primary" as a target_cluster so the connections should go
        // the the first Cluster as it's the Primary Cluster
        TargetClusterTestParams{/*target_cluster*/ "primary",
                                /*target_cluster_id*/ 0,
                                /*expected_connection_cluster_id*/ 0},
        // 1) we use first Cluster's GR UUID as a target_cluster so the
        // connections should go the the first Cluster
        TargetClusterTestParams{
            /*target_cluster*/ "00000000-0000-0000-0000-0000000000g1",
            /*target_cluster_id*/ 0,
            /*expected_connection_cluster_id*/ 0},
        // 2) we use second Cluster's GR UUID as a target_cluster so the
        // connections should go the the second Cluster
        TargetClusterTestParams{
            /*target_cluster*/ "00000000-0000-0000-0000-0000000000g2",
            /*target_cluster_id*/ 1,
            /*expected_connection_cluster_id*/ 1}));

struct TargetClusterChangeInMetataTestParams {
  // info about the target_cluster we start with (in config file)
  // and the expected connections destinations for that cluster
  TargetClusterTestParams initial_target_cluster;

  // info about the target_cluster we change to (in the metadata)
  // and the expected connections destinations for that cluster
  TargetClusterTestParams changed_target_cluster;

  // wether the initial connections (the ones for first target_cluster before
  // the change) are expected to be dropped or expected to stay
  bool initial_connections_should_drop;
};

class ClusterChangeTargetClusterInTheMetadataTest
    : public ClusterSetTest,
      public ::testing::WithParamInterface<
          TargetClusterChangeInMetataTestParams> {};

/**
 * @test Checks that the target cluster from the metadata overrides the one in
 * the static configuration file in the runtime.
 * [@FR3.7]
 * [@FR3.7.1]
 */
TEST_P(ClusterChangeTargetClusterInTheMetadataTest,
       ClusterChangeTargetClusterInTheMetadata) {
  const auto initial_target_cluster =
      GetParam().initial_target_cluster.target_cluster;
  const auto initial_target_cluster_id =
      GetParam().initial_target_cluster.target_cluster_id;
  const auto expected_initial_connection_cluster_id =
      GetParam().initial_target_cluster.expected_connection_cluster_id;

  create_clusterset(view_id, initial_target_cluster_id,
                    /*primary_cluster_id*/ 0, "metadata_clusterset.js",
                    /*router_options*/ R"({"targetCluster" : ")" +
                        initial_target_cluster + "\" }");
  /*auto &router =*/launch_router();

  SCOPED_TRACE(
      "// Make the connections to both RW and RO ports and check if they are "
      "directed to expected Cluster from the ClusterSet");
  std::unique_ptr<MySQLSession> rw_con1;
  if (expected_initial_connection_cluster_id == 0 /*primary_cluster_id*/) {
    rw_con1 = make_new_connection_ok(
        router_port_rw,
        clusterset_data_.clusters[expected_initial_connection_cluster_id]
            .nodes[kRWNodeId]
            .classic_port);
  } else {
    /* replica cluster, the RW connection should fail */
    verify_new_connection_fails(router_port_rw);
  }

  const auto first_ro_node1 =
      (expected_initial_connection_cluster_id == 0 /*Primary*/) ? kRONodeId
                                                                : kRWNodeId;
  auto ro_con1 = make_new_connection_ok(
      router_port_ro,
      clusterset_data_.clusters[expected_initial_connection_cluster_id]
          .nodes[first_ro_node1]
          .classic_port);

  SCOPED_TRACE(
      "// Change the target_cluster in the metadata of the first Cluster and "
      "bump its view id");

  const auto changed_target_cluster =
      GetParam().changed_target_cluster.target_cluster;
  const auto changed_target_cluster_id =
      GetParam().changed_target_cluster.target_cluster_id;

  set_mock_metadata(view_id, /*this_cluster_id*/ 0, changed_target_cluster_id,
                    clusterset_data_.clusters[0].nodes[0].http_port,
                    clusterset_data_,
                    /*router_options*/ R"({"targetCluster" : ")" +
                        changed_target_cluster + "\" }");

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  if (GetParam().initial_connections_should_drop) {
    SCOPED_TRACE(
        "// Since the target_cluster has changed the existing connection "
        "should get dropped");
    if (rw_con1) verify_existing_connection_dropped(rw_con1.get());
    verify_existing_connection_dropped(ro_con1.get());
  } else {
    if (rw_con1) verify_existing_connection_ok(rw_con1.get());
    verify_existing_connection_ok(ro_con1.get());
  }

  const auto expected_new_connection_cluster_id =
      GetParam().changed_target_cluster.expected_connection_cluster_id;

  SCOPED_TRACE(
      "// The new connections should get routed to the new target Cluster");

  if (expected_new_connection_cluster_id == 0 /*primary_cluster_id*/) {
    /*auto rw_con2 =*/make_new_connection_ok(
        router_port_rw,
        clusterset_data_.clusters[expected_new_connection_cluster_id]
            .nodes[kRWNodeId]
            .classic_port);
  } else {
    /* replica cluster, the RW connection should fail */
    verify_new_connection_fails(router_port_rw);
  }

  const auto first_ro_node =
      (expected_new_connection_cluster_id == 0 /*Primary*/) ? kRONodeId
                                                            : kRWNodeId;
  // +1 because it's round-robin and this is the second RO connection
  /*auto ro_con2 =*/make_new_connection_ok(
      router_port_ro,
      clusterset_data_.clusters[expected_new_connection_cluster_id]
          .nodes[first_ro_node + 1]
          .classic_port);

  SCOPED_TRACE(
      "// Check that only primary nodes from each Cluster were checked for the "
      "metadata");
  for (const auto &cluster : clusterset_data_.clusters) {
    unsigned node_id = 0;
    for (const auto &node : cluster.nodes) {
      const auto transactions_count = get_transaction_count(node.http_port);
      if (node_id == 0) {
        wait_for_transaction_count(node.http_port, 2);
      } else {
        // we expect the secondary node of each Cluster being queried only once,
        // when the first metadata refresh is run, as at that point we only have
        // a set of the metadata servers (all cluster nodes) from the state file
        // and we do not know which of then belongs to which of the Clusters
        // (we do not know the topology)
        EXPECT_EQ(transactions_count, 1);
      }
      ++node_id;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    ClusterChangeTargetClusterInTheMetadata,
    ClusterChangeTargetClusterInTheMetadataTest,
    ::testing::Values(
        // 0) "primary" (which is "gr-id-1") overwritten in metadata with
        // "gr-id-2" - existing connections are expected to drop
        TargetClusterChangeInMetataTestParams{
            TargetClusterTestParams{/*target_cluster*/ "primary",
                                    /*target_cluster_id*/ 0,
                                    /*expected_connection_cluster_id*/ 0},
            TargetClusterTestParams{
                /*target_cluster*/ "00000000-0000-0000-0000-0000000000g2",
                /*target_cluster_id*/ 1,
                /*expected_connection_cluster_id*/ 1},
            true},
        // 1) "gr-id-2" overwritten in metadata with "primary" - existing
        // connections are expected to drop
        TargetClusterChangeInMetataTestParams{
            TargetClusterTestParams{
                /*target_cluster*/ "00000000-0000-0000-0000-0000000000g2",
                /*target_cluster_id*/ 1,
                /*expected_connection_cluster_id*/ 1},
            TargetClusterTestParams{/*target_cluster*/ "primary",
                                    /*target_cluster_id*/ 0,
                                    /*expected_connection_cluster_id*/ 0},
            true},
        // 2) "gr-id-1" overwritten in metadata with "primary" - existing
        // connections are NOT expected to drop as this is the same Cluster
        TargetClusterChangeInMetataTestParams{
            TargetClusterTestParams{
                /*target_cluster*/ "00000000-0000-0000-0000-0000000000g1",
                /*target_cluster_id*/ 0,
                /*expected_connection_cluster_id*/ 0},
            TargetClusterTestParams{/*target_cluster*/ "primary",
                                    /*target_cluster_id*/ 0,
                                    /*expected_connection_cluster_id*/ 0},
            false}));

/**
 * @test Check that the Router correctly handles clustersetid not matching the
 * one in the state file.
 * [@FR13]
 * [@FR13.1]
 * [@TS_R14_1]
 */
TEST_F(ClusterSetTest, ClusterChangeClustersetIDInTheMetadata) {
  const int kTargetClusterId = 0;
  const std::string router_options = R"({"targetCluster" : "primary"})";

  create_clusterset(view_id, kTargetClusterId,
                    /*primary_cluster_id*/ 0, "metadata_clusterset.js",
                    router_options);
  /*auto &router =*/launch_router();

  SCOPED_TRACE(
      "// Make the connections to both RW and RO ports and check if they are"
      " directed to expected Cluster from the ClusterSet");
  auto rw_con1 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[kTargetClusterId]
                          .nodes[kRWNodeId]
                          .classic_port);
  auto ro_con1 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[kTargetClusterId]
                          .nodes[kRONodeId]
                          .classic_port);

  SCOPED_TRACE("// Change the clusterset_id in the metadata");
  clusterset_data_.uuid = "changed-clusterset-uuid";
  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      set_mock_metadata(view_id + 1, /*this_cluster_id*/ cluster.id,
                        kTargetClusterId, node.http_port, clusterset_data_,
                        router_options);
    }
  }
  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  SCOPED_TRACE(
      "// Check that the old connections got dropped and new are not "
      "possible");
  verify_existing_connection_dropped(rw_con1.get());
  verify_existing_connection_dropped(ro_con1.get());
  verify_new_connection_fails(router_port_rw);
  verify_new_connection_fails(router_port_ro);

  SCOPED_TRACE(
      "// Restore the original ClusterSet ID, matching the one stored in the "
      "state file");
  clusterset_data_.uuid = "clusterset-uuid";
  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      set_mock_metadata(view_id + 2, /*this_cluster_id*/ cluster.id,
                        kTargetClusterId, node.http_port, clusterset_data_,
                        router_options);
    }
  }
  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  SCOPED_TRACE("// Check that the connections are possible again");
  auto rw_con2 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[kTargetClusterId]
                          .nodes[kRWNodeId]
                          .classic_port);
  auto ro_con2 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[kTargetClusterId]
                          .nodes[kRONodeId + 1]
                          .classic_port);

  SCOPED_TRACE(
      "// Simulate the primary cluster can't be found in the ClusterSet");
  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      set_mock_metadata(view_id + 3, /*this_cluster_id*/ cluster.id,
                        kTargetClusterId, node.http_port, clusterset_data_,
                        router_options, "", {2, 1, 0},
                        /*simulate_cluster_not_found*/ true);
    }
  }
  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[1].nodes[0].http_port, 2));

  SCOPED_TRACE(
      "// Check that the old connections got dropped and new are not "
      "possible");
  verify_existing_connection_dropped(rw_con2.get());
  verify_existing_connection_dropped(ro_con2.get());
  verify_new_connection_fails(router_port_rw);
  verify_new_connection_fails(router_port_ro);
}

class UnknownClusterSetTargetClusterTest
    : public ClusterSetTest,
      public ::testing::WithParamInterface<TargetClusterTestParams> {};

/**
 * @test Checks that if the `target_cluster` for the Router can't be find in the
 * metadata the error should be logged and the Router should not accept any
 * connections.
 */
TEST_P(UnknownClusterSetTargetClusterTest, UnknownClusterSetTargetCluster) {
  const auto target_cluster = GetParam().target_cluster;
  const auto target_cluster_id = GetParam().target_cluster_id;

  create_clusterset(
      view_id, target_cluster_id, /*primary_cluster_id*/ 0,
      "metadata_clusterset.js",
      /*router_options*/ R"({"targetCluster" : ")" + target_cluster + "\" }");

  SCOPED_TRACE("// Prepare the dynamic state file for the Router");
  auto &router = launch_router(EXIT_SUCCESS, -1s);

  EXPECT_TRUE(wait_log_contains(router, GetParam().expected_error, 2s))
      << router.get_full_logfile();

  SCOPED_TRACE(
      "// Make the connections to both RW and RO ports, both should fail");

  verify_new_connection_fails(router_port_rw);
  verify_new_connection_fails(router_port_ro);
}

INSTANTIATE_TEST_SUITE_P(
    UnknownClusterSetTargetCluster, UnknownClusterSetTargetClusterTest,
    ::testing::Values(
        // [@TS_R9_1/1]
        TargetClusterTestParams{
            "000000000000000000000000000000g1", 0, 0,
            "ERROR.* Could not find target_cluster "
            "'000000000000000000000000000000g1' in the metadata"},
        // [@TS_R9_1/2]
        TargetClusterTestParams{
            "00000000-0000-0000-0000-0000000000g11", 0, 0,
            "ERROR.* Could not find target_cluster "
            "'00000000-0000-0000-0000-0000000000g11' in the metadata"},
        // [@TS_R9_1/3]
        TargetClusterTestParams{
            "00000000-0000-0000-0000-0000000000g", 0, 0,
            "ERROR.* Could not find target_cluster "
            "'00000000-0000-0000-0000-0000000000g' in the metadata"},
        // [@TS_R9_1/4]
        TargetClusterTestParams{
            "00000000-0000-0000-Z000-0000000000g1", 0, 0,
            "ERROR.* Could not find target_cluster "
            "'00000000-0000-0000-Z000-0000000000g1' in the metadata"},
        // [@TS_R9_1/5]
        TargetClusterTestParams{
            "00000000-0000-0000-0000-0000000000G1", 0, 0,
            "ERROR.* Could not find target_cluster "
            "'00000000-0000-0000-0000-0000000000G1' in the metadata"},
        // [@TS_R9_1/7]
        TargetClusterTestParams{"", 0, 0,
                                "Target cluster for router_id=1 not set, using "
                                "'primary' as a target cluster"},
        // [@TS_R9_1/8]
        TargetClusterTestParams{"0", 0, 0,
                                "ERROR.* Could not find target_cluster "
                                "'0' in the metadata"},
        // [@TS_R9_1/9]
        TargetClusterTestParams{
            "'00000000-0000-0000-0000-0000000000g1'", 0, 0,
            "ERROR.* Could not find target_cluster "
            "''00000000-0000-0000-0000-0000000000g1'' in the metadata"}));

/**
 * @test Check that the Router correctly follows primary Cluster when it is its
 * target_cluster.
 */
TEST_F(ClusterSetTest, ClusterRolesChangeInTheRuntime) {
  // first cluster is a primary on start
  unsigned primary_cluster_id = 0;
  const std::string router_options = R"({"targetCluster" : "primary"})";

  create_clusterset(view_id, /*target_cluster_id*/ primary_cluster_id,
                    /*primary_cluster_id*/ primary_cluster_id,
                    "metadata_clusterset.js", router_options);
  /*auto &router =*/launch_router();

  SCOPED_TRACE(
      "// Make the connections to both RW and RO ports and check if they are"
      " directed to expected Cluster from the ClusterSet");
  auto rw_con1 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[primary_cluster_id]
                          .nodes[kRWNodeId]
                          .classic_port);
  auto ro_con1 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[primary_cluster_id]
                          .nodes[kRONodeId]
                          .classic_port);

  verify_only_primary_gets_updates(primary_cluster_id);

  ////////////////////////////////////
  SCOPED_TRACE(
      "// Change the primary cluster in the metadata, now the first Replica "
      "Cluster becomes the PRIMARY");
  ////////////////////////////////////

  view_id++;
  primary_cluster_id = 1;
  change_clusterset_primary(clusterset_data_, primary_cluster_id);
  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      const auto http_port = node.http_port;
      set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                        /*target_cluster_id*/ primary_cluster_id, http_port,
                        clusterset_data_, router_options);
    }
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  SCOPED_TRACE("// Check that the existing connections got dropped");
  verify_existing_connection_dropped(rw_con1.get());
  verify_existing_connection_dropped(ro_con1.get());

  SCOPED_TRACE(
      "// Check that new connections are directed to the new PRIMARY cluster "
      "nodes");
  auto rw_con2 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[primary_cluster_id]
                          .nodes[kRWNodeId]
                          .classic_port);
  // +1%2 is for round-robin
  auto ro_con2 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[primary_cluster_id]
                          .nodes[kRONodeId + 1 % 2]
                          .classic_port);

  // check the new primary gets updates
  verify_only_primary_gets_updates(primary_cluster_id);

  ////////////////////////////////////
  SCOPED_TRACE(
      "// Change the primary cluster in the metadata, now the second Replica "
      "Cluster becomes the PRIMARY");
  ////////////////////////////////////
  view_id++;
  primary_cluster_id = 2;
  change_clusterset_primary(clusterset_data_, primary_cluster_id);
  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      const auto http_port = node.http_port;
      set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                        /*target_cluster_id*/ primary_cluster_id, http_port,
                        clusterset_data_, router_options);
    }
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  SCOPED_TRACE("// Check that the existing connections got dropped");
  verify_existing_connection_dropped(rw_con2.get());
  verify_existing_connection_dropped(ro_con2.get());

  SCOPED_TRACE(
      "// Check that new connections are directed to the new PRIMARY cluster "
      "nodes");
  auto rw_con3 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[primary_cluster_id]
                          .nodes[kRWNodeId]
                          .classic_port);
  // +2 %2 is for round-robin
  auto ro_con3 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[primary_cluster_id]
                          .nodes[kRONodeId + 2 % 2]
                          .classic_port);

  ////////////////////////////////////
  SCOPED_TRACE(
      "// Change the primary cluster in the metadata, let the original PRIMARY "
      "be the primary again");
  ////////////////////////////////////
  view_id++;
  primary_cluster_id = 0;
  change_clusterset_primary(clusterset_data_, primary_cluster_id);
  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      const auto http_port = node.http_port;
      set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                        /*target_cluster_id*/ primary_cluster_id, http_port,
                        clusterset_data_, router_options);
    }
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  SCOPED_TRACE("// Check that the existing connections got dropped");
  verify_existing_connection_dropped(rw_con3.get());
  verify_existing_connection_dropped(ro_con3.get());

  SCOPED_TRACE(
      "// Check that new connections are directed to the new PRIMARY cluster "
      "nodes");
  auto rw_con4 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[primary_cluster_id]
                          .nodes[kRWNodeId]
                          .classic_port);
  // +3%2 is for round-robin
  auto ro_con4 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[primary_cluster_id]
                          .nodes[kRONodeId + 3 % 2]
                          .classic_port);
}

/**
 * @test Check that the Router sticks to the target_cluster given by UUID when
 * its role changes starting from PRIMARY.
 * [@TS_R6_2]
 */
TEST_F(ClusterSetTest, TargetClusterStickToPrimaryUUID) {
  // first cluster is a primary on start
  unsigned primary_cluster_id = 0;
  const unsigned target_cluster_id = 0;
  const std::string router_options =
      R"({"targetCluster" : "00000000-0000-0000-0000-0000000000g1"})";

  create_clusterset(view_id, /*target_cluster_id*/ target_cluster_id,
                    /*primary_cluster_id*/ primary_cluster_id,
                    "metadata_clusterset.js", router_options);
  /*auto &router =*/launch_router();

  SCOPED_TRACE(
      "// Make the connections to both RW and RO ports and check if they are"
      " directed to expected Cluster from the ClusterSet");
  auto rw_con1 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[target_cluster_id]
                          .nodes[kRWNodeId]
                          .classic_port);
  auto ro_con1 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[target_cluster_id]
                          .nodes[kRONodeId]
                          .classic_port);

  // check that the primary cluster is getting the periodic metadata updates
  verify_only_primary_gets_updates(primary_cluster_id);

  ////////////////////////////////////
  SCOPED_TRACE(
      "// Change the primary cluster in the metadata, now the first Replica "
      "Cluster becomes the PRIMARY");
  ////////////////////////////////////

  view_id++;
  primary_cluster_id = 1;
  change_clusterset_primary(clusterset_data_, primary_cluster_id);
  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      const auto http_port = node.http_port;
      set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                        /*target_cluster_id*/ target_cluster_id, http_port,
                        clusterset_data_, router_options);
    }
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  SCOPED_TRACE(
      "// RW connection should get dropped as our target_cluster is no longer "
      "PRIMARY");
  verify_existing_connection_dropped(rw_con1.get());
  SCOPED_TRACE("// RO connection should stay valid");
  verify_existing_connection_ok(ro_con1.get());

  SCOPED_TRACE(
      "// Check that new RO connection is directed to the same Cluster and no "
      "new RW connection is possible");
  // +1%3 because we round-robin and we now have 3 RO nodes
  auto ro_con2 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[target_cluster_id]
                          .nodes[(kRWNodeId + 1) % 3]
                          .classic_port);
  verify_new_connection_fails(router_port_rw);

  // check that the primary cluster is getting the periodic metadata updates
  verify_only_primary_gets_updates(primary_cluster_id);

  ////////////////////////////////////
  SCOPED_TRACE(
      "// Change the primary cluster in the metadata, now the second Replica "
      "Cluster becomes the PRIMARY");
  ////////////////////////////////////
  view_id++;
  primary_cluster_id = 2;
  change_clusterset_primary(clusterset_data_, primary_cluster_id);
  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      const auto http_port = node.http_port;
      set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                        /*target_cluster_id*/ target_cluster_id, http_port,
                        clusterset_data_, router_options);
    }
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  SCOPED_TRACE("// Both existing RO connections should be fine");
  verify_existing_connection_ok(ro_con1.get());
  verify_existing_connection_ok(ro_con2.get());

  SCOPED_TRACE(
      "// Check that new RO connection is directed to the same Cluster and no "
      "new RW connection is possible");
  verify_new_connection_fails(router_port_rw);
  // +2%3 because we round-robin and we now have 3 RO nodes
  auto ro_con3 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[target_cluster_id]
                          .nodes[(kRWNodeId + 2) % 3]
                          .classic_port);

  ////////////////////////////////////
  SCOPED_TRACE(
      "// Change the primary cluster in the metadata, let the original PRIMARY "
      "be the primary again");
  ////////////////////////////////////
  view_id++;
  primary_cluster_id = 0;
  change_clusterset_primary(clusterset_data_, primary_cluster_id);
  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      const auto http_port = node.http_port;
      set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                        /*target_cluster_id*/ target_cluster_id, http_port,
                        clusterset_data_, router_options);
    }
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  SCOPED_TRACE("// Check that all the existing RO connections are OK");
  verify_existing_connection_ok(ro_con1.get());
  verify_existing_connection_ok(ro_con2.get());
  verify_existing_connection_ok(ro_con3.get());

  SCOPED_TRACE("// Check that both RW and RO connections are possible again");
  auto rw_con4 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[target_cluster_id]
                          .nodes[kRWNodeId]
                          .classic_port);
  auto ro_con4 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[target_cluster_id]
                          .nodes[kRONodeId]
                          .classic_port);
}

/**
 * @test Check that the Router sticks to the target_cluster given by UUID when
 * its role changes starting from REPLICA.
 */
TEST_F(ClusterSetTest, TargetClusterStickToReaplicaUUID) {
  // first cluster is a primary on start
  unsigned primary_cluster_id = 0;
  // our target_cluster is first Replica
  const unsigned target_cluster_id = 1;
  const std::string router_options =
      R"({"targetCluster" : "00000000-0000-0000-0000-0000000000g2"})";

  create_clusterset(view_id, /*target_cluster_id*/ target_cluster_id,
                    /*primary_cluster_id*/ primary_cluster_id,
                    "metadata_clusterset.js", router_options);

  /*auto &router =*/launch_router();

  SCOPED_TRACE(
      "// Make the connections to both RW and RO ports, RW should not be "
      "possible as our target_cluster is REPLICA cluster, RO should be routed "
      "to our target_cluster");
  verify_new_connection_fails(router_port_rw);
  auto ro_con1 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[target_cluster_id]
                          .nodes[kRWNodeId]
                          .classic_port);

  ////////////////////////////////////
  SCOPED_TRACE(
      "// Change the primary cluster in the metadata, now the SECOND REPLICA "
      "Cluster becomes the PRIMARY");
  ////////////////////////////////////

  view_id++;
  primary_cluster_id = 2;
  change_clusterset_primary(clusterset_data_, primary_cluster_id);
  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      const auto http_port = node.http_port;
      set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                        /*target_cluster_id*/ target_cluster_id, http_port,
                        clusterset_data_, router_options);
    }
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  SCOPED_TRACE("// Our existing RO connection should still be fine");
  verify_existing_connection_ok(ro_con1.get());

  SCOPED_TRACE(
      "// Check that new RO connection is directed to the same Cluster and no "
      "new RW connection is possible");
  // +1%3 because we round-robin and we now have 3 RO nodes
  auto ro_con2 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[target_cluster_id]
                          .nodes[(kRWNodeId + 1) % 3]
                          .classic_port);
  verify_new_connection_fails(router_port_rw);

  ////////////////////////////////////
  SCOPED_TRACE(
      "// Change the primary cluster in the metadata, now the FIRST REPLICA "
      "which happens to be our target cluster becomes PRIMARY");
  ////////////////////////////////////
  view_id++;
  primary_cluster_id = 1;
  change_clusterset_primary(clusterset_data_, primary_cluster_id);
  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      const auto http_port = node.http_port;
      set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                        /*target_cluster_id*/ target_cluster_id, http_port,
                        clusterset_data_, router_options);
    }
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  SCOPED_TRACE("// Both existing RO connections should be fine");
  verify_existing_connection_ok(ro_con1.get());
  verify_existing_connection_ok(ro_con2.get());

  SCOPED_TRACE(
      "// Check that new RO connection is directed to the same Cluster and now "
      "RW connection is possible");
  auto rw_con = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[target_cluster_id]
                          .nodes[kRWNodeId]
                          .classic_port);
  // +2%2 because we round-robin and we now have 2 RO nodes
  auto ro_con3 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[target_cluster_id]
                          .nodes[kRONodeId + 2 % 2]
                          .classic_port);

  ////////////////////////////////////
  SCOPED_TRACE(
      "// Change the primary cluster in the metadata, let the original PRIMARY "
      "be the primary again");
  ////////////////////////////////////
  view_id++;
  primary_cluster_id = 0;
  change_clusterset_primary(clusterset_data_, primary_cluster_id);
  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      const auto http_port = node.http_port;
      set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                        /*target_cluster_id*/ target_cluster_id, http_port,
                        clusterset_data_, router_options);
    }
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  SCOPED_TRACE("// Check that all the existing RO connections are OK");
  verify_existing_connection_ok(ro_con1.get());
  verify_existing_connection_ok(ro_con2.get());
  verify_existing_connection_ok(ro_con3.get());
  SCOPED_TRACE(
      "// Check that RW connection got dropped as our target_cluster is not "
      "PRIMARY anymore");
  verify_existing_connection_dropped(rw_con.get());

  SCOPED_TRACE(
      "// Check that new RO connection is possible, new RW connection is not "
      "possible");
  verify_new_connection_fails(router_port_rw);
  auto ro_con4 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[target_cluster_id]
                          .nodes[kRONodeId]
                          .classic_port);
}

class ViewIdChangesTest
    : public ClusterSetTest,
      public ::testing::WithParamInterface<TargetClusterTestParams> {};

/**
 * @test Check that the Router correctly notices the view_id changes and
 * applies the new metadata according to them.
 * [@FR8]
 * [@FR8.1]
 */
TEST_P(ViewIdChangesTest, ViewIdChanges) {
  const int target_cluster_id = GetParam().target_cluster_id;
  const std::string target_cluster = GetParam().target_cluster;
  const std::string router_options =
      R"({"targetCluster" : ")" + target_cluster + "\" }";

  SCOPED_TRACE(
      "// We start wtih view_id=1, all the clusterset nodes are metadata "
      "servers");

  create_clusterset(view_id, target_cluster_id,
                    /*primary_cluster_id*/ 0, "metadata_clusterset.js",
                    router_options);
  /*auto &router =*/launch_router();
  EXPECT_EQ(9u, clusterset_data_.get_all_nodes_classic_ports().size());

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  check_state_file(router_state_file, mysqlrouter::ClusterType::GR_CS,
                   clusterset_data_.uuid,
                   clusterset_data_.get_all_nodes_classic_ports(), view_id);

  SCOPED_TRACE(
      "// Now let's make some change in the metadata (remove second node in "
      "the second replicaset) and let know only first REPLICA cluster about "
      "that");

  clusterset_data_.remove_node("00000000-0000-0000-0000-000000000033");
  EXPECT_EQ(8u, clusterset_data_.get_all_nodes_classic_ports().size());

  set_mock_metadata(view_id + 1, /*this_cluster_id*/ 1, target_cluster_id,
                    clusterset_data_.clusters[1].nodes[0].http_port,
                    clusterset_data_, router_options);

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  SCOPED_TRACE(
      "// Check that the Router has seen the change and that it is reflected "
      "in the state file");

  check_state_file(router_state_file, mysqlrouter::ClusterType::GR_CS,
                   clusterset_data_.uuid,
                   clusterset_data_.get_all_nodes_classic_ports(), view_id + 1);

  SCOPED_TRACE(
      "// Let's make another change in the metadata (remove second node in "
      "the first replicaset) and let know only second REPLICA cluster about "
      "that");

  clusterset_data_.remove_node("00000000-0000-0000-0000-000000000023");
  EXPECT_EQ(7u, clusterset_data_.get_all_nodes_classic_ports().size());

  set_mock_metadata(view_id + 2, /*this_cluster_id*/ 2, target_cluster_id,
                    clusterset_data_.clusters[2].nodes[0].http_port,
                    clusterset_data_, router_options);

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  SCOPED_TRACE(
      "// Check that the Router has seen the change and that it is reflected "
      "in the state file");

  check_state_file(router_state_file, mysqlrouter::ClusterType::GR_CS,
                   clusterset_data_.uuid,
                   clusterset_data_.get_all_nodes_classic_ports(), view_id + 2);

  SCOPED_TRACE(
      "// Let's propagete the last change to all nodes in the ClusterSet");

  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      const auto http_port = node.http_port;
      set_mock_metadata(view_id + 2, /*this_cluster_id*/ 2, target_cluster_id,
                        http_port, clusterset_data_, router_options);
    }
  }

  // state file should not change
  SCOPED_TRACE(
      "// Check that the Router has seen the change and that it is reflected "
      "in the state file");

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[0].nodes[0].http_port, 2));

  check_state_file(router_state_file, mysqlrouter::ClusterType::GR_CS,
                   clusterset_data_.uuid,
                   clusterset_data_.get_all_nodes_classic_ports(), view_id + 2);
}

INSTANTIATE_TEST_SUITE_P(ViewIdChanges, ViewIdChangesTest,
                         ::testing::Values(
                             // [@TS_R11_1] // TODO: are these references ok?
                             TargetClusterTestParams{"primary", 0},
                             // [@TS_R11_2]
                             TargetClusterTestParams{
                                 "00000000-0000-0000-0000-0000000000g2", 1}));

/**
 * @test Check that when 2 clusters claim they are both PRIMARY, Router follows
 * the one that has a highier view_id
 * [@FR9]
 * [@TS_R11_3]
 */
TEST_F(ClusterSetTest, TwoPrimaryClustersHighierViewId) {
  const std::string router_options = R"({"targetCluster" : "primary"})";
  SCOPED_TRACE(
      "// We configure Router to follow PRIMARY cluster, first cluster starts "
      "as a PRIMARY");

  create_clusterset(view_id, /*target_cluster_id*/ 0,
                    /*primary_cluster_id*/ 0, "metadata_clusterset.js",
                    router_options);
  /*auto &router =*/launch_router();

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[kPrimaryClusterId].nodes[0].http_port, 2));

  auto rw_con1 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[kPrimaryClusterId]
                          .nodes[kRWNodeId]
                          .classic_port);

  auto ro_con1 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[kPrimaryClusterId]
                          .nodes[kRONodeId]
                          .classic_port);

  SCOPED_TRACE(
      "// Now let's make first REPLICA to claim that it's also a primary. But "
      "it has a highier view so the Router should believe the REPLICA");

  change_clusterset_primary(clusterset_data_, kFirstReplicaClusterId);
  for (unsigned node_id = 0;
       node_id < clusterset_data_.clusters[kFirstReplicaClusterId].nodes.size();
       ++node_id) {
    set_mock_metadata(view_id + 1, /*this_cluster_id*/ kFirstReplicaClusterId,
                      kFirstReplicaClusterId,
                      clusterset_data_.clusters[kFirstReplicaClusterId]
                          .nodes[node_id]
                          .http_port,
                      clusterset_data_, router_options);
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[kFirstReplicaClusterId].nodes[0].http_port, 2));

  SCOPED_TRACE(
      "// Check that the Router has seen the change and that it is reflected "
      "in the state file");

  check_state_file(router_state_file, mysqlrouter::ClusterType::GR_CS,
                   clusterset_data_.uuid,
                   clusterset_data_.get_all_nodes_classic_ports(), view_id + 1);

  SCOPED_TRACE(
      "// Check that the Router now uses new PRIMARY as a target cluster - "
      "existing connections dropped, new one directed to second Cluster");

  verify_existing_connection_dropped(rw_con1.get());
  verify_existing_connection_dropped(ro_con1.get());

  auto rw_con2 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[kFirstReplicaClusterId]
                          .nodes[kRWNodeId]
                          .classic_port);

  // +1 as we round-dobin and this is already a second connection
  auto ro_con2 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[kFirstReplicaClusterId]
                          .nodes[kRONodeId + 1]
                          .classic_port);

  SCOPED_TRACE(
      "// Now let's bump the old PRIMARY's view_id up, it should become again "
      "our target_cluster");

  change_clusterset_primary(clusterset_data_, kPrimaryClusterId);
  for (unsigned node_id = 0;
       node_id < clusterset_data_.clusters[kPrimaryClusterId].nodes.size();
       ++node_id) {
    set_mock_metadata(view_id + 2, /*this_cluster_id*/ kPrimaryClusterId,
                      kPrimaryClusterId,
                      clusterset_data_.clusters[kFirstReplicaClusterId]
                          .nodes[node_id]
                          .http_port,
                      clusterset_data_, router_options);
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[kPrimaryClusterId].nodes[0].http_port, 2));

  SCOPED_TRACE(
      "// Check that the Router has seen the change and that it is reflected "
      "in the state file");

  check_state_file(router_state_file, mysqlrouter::ClusterType::GR_CS,
                   clusterset_data_.uuid,
                   clusterset_data_.get_all_nodes_classic_ports(), view_id + 2);

  SCOPED_TRACE(
      "// Check that the Router now uses original PRIMARY as a target cluster "
      "- "
      "existing connections dropped, new one directed to first Cluster");

  verify_existing_connection_dropped(rw_con2.get());
  verify_existing_connection_dropped(ro_con2.get());

  /*auto rw_con3 =*/make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[kPrimaryClusterId]
                          .nodes[kRWNodeId]
                          .classic_port);

  // +1 as we round-dobin and this is already a second connection
  /*auto ro_con3 =*/make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[kPrimaryClusterId]
                          .nodes[kRONodeId]
                          .classic_port);
}

/**
 * @test Check that when 2 clusters claim they are both PRIMARY, Router follows
 * the one that has a highier view_id
 * [@FR9]
 * [@TS_R11_4]
 */
TEST_F(ClusterSetTest, TwoPrimaryClustersLowerViewId) {
  view_id = 1;

  SCOPED_TRACE(
      "// We configure Router to follow PRIMARY cluster, first cluster starts "
      "as a PRIMARY");

  create_clusterset(view_id, /*target_cluster_id*/ 0,
                    /*primary_cluster_id*/ 0, "metadata_clusterset.js",
                    /*router_options*/ R"({"targetCluster" : "primary"})");
  /*auto &router =*/launch_router();

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[kPrimaryClusterId].nodes[0].http_port, 2));

  auto rw_con1 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[kPrimaryClusterId]
                          .nodes[kRWNodeId]
                          .classic_port);

  auto ro_con1 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[kPrimaryClusterId]
                          .nodes[kRONodeId]
                          .classic_port);

  SCOPED_TRACE(
      "// Now let's make first REPLICA to claim that it's also a primary. But "
      "it has a lower view so the Router should not take that into account");

  change_clusterset_primary(clusterset_data_, kFirstReplicaClusterId);
  for (unsigned node_id = 0;
       node_id < clusterset_data_.clusters[kFirstReplicaClusterId].nodes.size();
       ++node_id) {
    set_mock_metadata(view_id - 1, /*this_cluster_id*/ kFirstReplicaClusterId,
                      kFirstReplicaClusterId,
                      clusterset_data_.clusters[kFirstReplicaClusterId]
                          .nodes[node_id]
                          .http_port,
                      clusterset_data_,
                      /*router_options*/ "");
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[kFirstReplicaClusterId].nodes[0].http_port, 2));

  SCOPED_TRACE("// Check that the state file did not change");

  check_state_file(router_state_file, mysqlrouter::ClusterType::GR_CS,
                   clusterset_data_.uuid,
                   clusterset_data_.get_all_nodes_classic_ports(), view_id);

  SCOPED_TRACE(
      "// Check that existing connections are still open and the original "
      "PRIMARY is used for new ones");

  verify_existing_connection_ok(rw_con1.get());
  verify_existing_connection_ok(ro_con1.get());

  auto rw_con2 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[kPrimaryClusterId]
                          .nodes[kRWNodeId]
                          .classic_port);

  // +1 as we round-dobin and this is already a second connection
  auto ro_con2 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[kPrimaryClusterId]
                          .nodes[kRONodeId + 1]
                          .classic_port);
}

struct InvalidatedClusterTestParams {
  std::string invalidated_cluster_routing_policy;
  bool expected_ro_connections_allowed;
};

class ClusterMarkedInvalidInTheMetadataTest
    : public ClusterSetTest,
      public ::testing::WithParamInterface<InvalidatedClusterTestParams> {};

/**
 * @test Check that when target_cluster is marked as invalidated in the metadata
 * the Router either handles only RO connections or no connections at all
 * depending on the invalidatedClusterRoutingPolicy
 * [@FR11]
 * [@TS_R15_1-3]
 */
TEST_P(ClusterMarkedInvalidInTheMetadataTest,
       ClusterMarkedInvalidInTheMetadata) {
  view_id = 1;
  const std::string policy = GetParam().invalidated_cluster_routing_policy;
  const bool ro_allowed = GetParam().expected_ro_connections_allowed;

  SCOPED_TRACE("// We configure Router to follow the first REPLICA cluster");

  create_clusterset(view_id, /*target_cluster_id*/ kPrimaryClusterId,
                    /*primary_cluster_id*/ kPrimaryClusterId,
                    "metadata_clusterset.js",
                    /*router_options*/ R"({"targetCluster" : "primary"})");
  /* auto &router = */ launch_router();

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[kPrimaryClusterId].nodes[0].http_port, 2));

  auto rw_con1 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[kPrimaryClusterId]
                          .nodes[kRWNodeId]
                          .classic_port);

  auto ro_con1 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[kPrimaryClusterId]
                          .nodes[kRONodeId]
                          .classic_port);

  SCOPED_TRACE(
      "// Mark our PRIMARY cluster as invalidated in the metadata, also set "
      "the selected invalidatedClusterRoutingPolicy");
  clusterset_data_.clusters[kPrimaryClusterId].invalid = true;
  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      const auto http_port = node.http_port;
      set_mock_metadata(
          view_id + 1, /*this_cluster_id*/ cluster.id,
          /*target_cluster_id*/ kPrimaryClusterId, http_port, clusterset_data_,
          /*router_options*/
          R"({"targetCluster" : "primary", "invalidatedClusterRoutingPolicy" : ")" +
              policy + "\" }");
    }
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[kPrimaryClusterId].nodes[0].http_port, 2));

  SCOPED_TRACE(
      "// Check that existing RW connections are down and no new are possible");

  verify_existing_connection_dropped(rw_con1.get());
  verify_new_connection_fails(router_port_rw);

  SCOPED_TRACE(
      "// Check that RO connections are possible or not depending on the "
      "configured policy");
  if (!ro_allowed) {
    verify_existing_connection_dropped(ro_con1.get());
    verify_new_connection_fails(router_port_ro);
  } else {
    verify_existing_connection_ok(ro_con1.get());
    make_new_connection_ok(router_port_ro,
                           clusterset_data_.clusters[kPrimaryClusterId]
                               .nodes[kRONodeId]
                               .classic_port);
  }
}

INSTANTIATE_TEST_SUITE_P(
    ClusterMarkedInvalidInTheMetadata, ClusterMarkedInvalidInTheMetadataTest,
    ::testing::Values(
        // policy empty, default should be dropAll so RO connections are not
        // allowed
        InvalidatedClusterTestParams{"", false},
        // unsupported policy name, again expect the default behavior
        InvalidatedClusterTestParams{"unsupported", false},
        // explicitly set dropAll, no RO connections allowed again
        InvalidatedClusterTestParams{"drop_all", false},
        // accept_ro policy in  the metadata, RO connections are allowed
        InvalidatedClusterTestParams{"accept_ro", true}));

/**
 * @test Check that the Router correctly follows new target_cluster when it is
 * changed manually in the config file.
 * [@FR3.6]
 * [@TS_R8_3]
 */
#if 0
TEST_F(ClusterSetTest, ManualTargetClusterChangeInConfig) {
  create_clusterset(0, /*target_cluster_id*/ 0,
                    /*primary_cluster_id*/ 0, "metadata_clusterset.js");

  SCOPED_TRACE("// Launch Router with target_cluster=primary");
  auto &router = launch_router("primary");

  make_new_connection_ok(router_port_rw,
                         clusterset_data_.clusters[kPrimaryClusterId]
                             .nodes[kRWNodeId]
                             .classic_port);

  make_new_connection_ok(router_port_ro,
                         clusterset_data_.clusters[kPrimaryClusterId]
                             .nodes[kRONodeId]
                             .classic_port);

  router.kill();
  EXPECT_TRUE(
      wait_log_contains(router, "DEBUG .* Unloading all plugins.", 5000ms));

  SCOPED_TRACE(
      "// Now the Router is down, let's change the target_cluster to first "
      "Replica Cluster");

  {
    std::ifstream conf_in{router_conf_file};
    std::ofstream conf_out{router_conf_file + "tmp"};
    std::string line;
    bool replaced{false};
    EXPECT_TRUE(conf_in);
    while (std::getline(conf_in, line)) {
      if (line == "target_cluster=primary") {
        conf_out << "target_cluster=00000000-0000-0000-0000-0000000000g2"
                 << std::endl;
        replaced = true;
      } else {
        conf_out << line << std::endl;
      }
    }
    EXPECT_TRUE(replaced);
  }
  mysqlrouter::rename_file(router_conf_file + "tmp", router_conf_file);

  for (const auto &cluster : clusterset_data_.clusters) {
    for (const auto &node : cluster.nodes) {
      const auto http_port = node.http_port;
      set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                        /*target_cluster_id*/ kFirstReplicaClusterId, http_port,
                        clusterset_data_,
                        /*router_options*/ "");
    }
  }

  SCOPED_TRACE(
      "// After restarting the Router it should now use Replica for a "
      "connections");
  relaunch_router(router_conf_file);

  verify_new_connection_fails(router_port_rw);

  make_new_connection_ok(router_port_ro,
                         clusterset_data_.clusters[kFirstReplicaClusterId]
                             .nodes[kRWNodeId]
                             .classic_port);
}
#endif

/**
 * @test Check that the changes to the ClusterSet topology are reflected in the
 * state file in the runtime.
 * [@FR12]
 * [@TS_R13_1]
 */
TEST_F(ClusterSetTest, StateFileMetadataServersChange) {
  // also check if we handle view_id grater than 2^32 correctly
  uint64_t view_id = std::numeric_limits<uint32_t>::max() + 1;
  const std::string router_options = R"({"targetCluster" : "primary"})";
  create_clusterset(view_id, /*target_cluster_id*/ kPrimaryClusterId,
                    /*primary_cluster_id*/ 0, "metadata_clusterset.js",
                    router_options);

  const auto original_clusterset_data = clusterset_data_;

  SCOPED_TRACE("// Launch Router with target_cluster=primary");
  /*auto &router =*/launch_router();

  check_state_file(router_state_file, mysqlrouter::ClusterType::GR_CS,
                   clusterset_data_.uuid,
                   clusterset_data_.get_all_nodes_classic_ports(), view_id);

  SCOPED_TRACE(
      "// Remove second Replica Cluster nodes one by one and check that it is "
      "reflected in the state file");

  for (unsigned node_id = 1; node_id <= 3; ++node_id) {
    // remove node from the metadata
    clusterset_data_.remove_node("00000000-0000-0000-0000-00000000003" +
                                 std::to_string(node_id));
    ++view_id;
    // update each remaining node with that metadata
    for (const auto &cluster : clusterset_data_.clusters) {
      for (const auto &node : cluster.nodes) {
        const auto http_port = node.http_port;
        set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                          /*target_cluster_id*/ kPrimaryClusterId, http_port,
                          clusterset_data_, router_options);
      }
    }

    // wait for the Router to refresh the metadata
    EXPECT_TRUE(wait_for_transaction_count_increase(
        clusterset_data_.clusters[kPrimaryClusterId].nodes[0].http_port, 2));

    // check that the list of the nodes is reflected in the state file
    EXPECT_EQ(9 - node_id,
              clusterset_data_.get_all_nodes_classic_ports().size());
    check_state_file(router_state_file, mysqlrouter::ClusterType::GR_CS,
                     clusterset_data_.uuid,
                     clusterset_data_.get_all_nodes_classic_ports(), view_id);
  }

  SCOPED_TRACE("// Check that we can still connect to the Primary");
  make_new_connection_ok(router_port_rw,
                         clusterset_data_.clusters[kPrimaryClusterId]
                             .nodes[kRWNodeId]
                             .classic_port);

  make_new_connection_ok(router_port_ro,
                         clusterset_data_.clusters[kPrimaryClusterId]
                             .nodes[kRONodeId]
                             .classic_port);

  SCOPED_TRACE(
      "// Remove Primary Cluster nodes one by one and check that it is "
      "reflected in the state file");

  for (unsigned node_id = 1; node_id <= 3; ++node_id) {
    // remove node from the metadata
    clusterset_data_.remove_node("00000000-0000-0000-0000-00000000001" +
                                 std::to_string(node_id));
    ++view_id;
    // update each remaining node with that metadata
    for (const auto &cluster : clusterset_data_.clusters) {
      for (const auto &node : cluster.nodes) {
        const auto http_port = node.http_port;
        set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                          /*target_cluster_id*/ kPrimaryClusterId, http_port,
                          clusterset_data_, router_options);
      }
    }

    // wait for the Router to refresh the metadata
    EXPECT_TRUE(wait_for_transaction_count_increase(
        clusterset_data_.clusters[kFirstReplicaClusterId].nodes[0].http_port,
        2));

    // check that the list of the nodes is reflected in the state file
    EXPECT_EQ(9 - 3 - node_id,
              clusterset_data_.get_all_nodes_classic_ports().size());
    check_state_file(router_state_file, mysqlrouter::ClusterType::GR_CS,
                     clusterset_data_.uuid,
                     clusterset_data_.get_all_nodes_classic_ports(), view_id);
  }

  verify_new_connection_fails(router_port_rw);
  verify_new_connection_fails(router_port_ro);

  SCOPED_TRACE(
      "// Remove First Replica Cluster nodes one by one and check that it is "
      "reflected in the state file");

  for (unsigned node_id = 2; node_id <= 3; ++node_id) {
    // remove node from the metadata
    clusterset_data_.remove_node("00000000-0000-0000-0000-00000000002" +
                                 std::to_string(node_id));
    ++view_id;
    // update each remaining node with that metadata
    for (const auto &cluster : clusterset_data_.clusters) {
      for (const auto &node : cluster.nodes) {
        const auto http_port = node.http_port;
        set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                          /*target_cluster_id*/ kPrimaryClusterId, http_port,
                          clusterset_data_, router_options);
      }
    }

    // wait for the Router to refresh the metadata
    EXPECT_TRUE(wait_for_transaction_count_increase(
        clusterset_data_.clusters[kFirstReplicaClusterId].nodes[0].http_port,
        2));

    // check that the list of the nodes is reflected in the state file
    EXPECT_EQ(4 - node_id,
              clusterset_data_.get_all_nodes_classic_ports().size());

    check_state_file(router_state_file, mysqlrouter::ClusterType::GR_CS,
                     clusterset_data_.uuid,
                     clusterset_data_.get_all_nodes_classic_ports(), view_id);
  }

  SCOPED_TRACE(
      "// Remove the last node, that should not be reflected in the state file "
      "as Router never writes empty list to the state file");
  clusterset_data_.remove_node("00000000-0000-0000-0000-000000000021");
  view_id++;
  set_mock_metadata(
      view_id, /*this_cluster_id*/ 1,
      /*target_cluster_id*/ kPrimaryClusterId,
      clusterset_data_.clusters[kFirstReplicaClusterId].nodes[0].http_port,
      clusterset_data_, router_options);
  // wait for the Router to refresh the metadata
  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[kFirstReplicaClusterId].nodes[0].http_port, 2));

  // check that the list of the nodes is NOT reflected in the state file
  EXPECT_EQ(0, clusterset_data_.get_all_nodes_classic_ports().size());
  const std::vector<uint16_t> expected_port{
      clusterset_data_.clusters[kFirstReplicaClusterId].nodes[0].classic_port};
  check_state_file(router_state_file, mysqlrouter::ClusterType::GR_CS,
                   clusterset_data_.uuid, expected_port, view_id - 1);

  verify_new_connection_fails(router_port_rw);
  verify_new_connection_fails(router_port_ro);

  SCOPED_TRACE("// Restore Primary Cluster nodes one by one");

  for (unsigned node_id = 1; node_id <= 3; ++node_id) {
    ++view_id;
    clusterset_data_.add_node(
        kPrimaryClusterId, original_clusterset_data.clusters[kPrimaryClusterId]
                               .nodes[node_id - 1]);
    // update each node with that metadata
    for (const auto &cluster : clusterset_data_.clusters) {
      for (const auto &node : cluster.nodes) {
        const auto http_port = node.http_port;
        set_mock_metadata(view_id, /*this_cluster_id*/ cluster.id,
                          /*target_cluster_id*/ kPrimaryClusterId, http_port,
                          clusterset_data_, router_options);
      }
    }

    // if this is the first node that we are adding back we also need to set it
    // in our last standing metadata server which is no longer part of the
    // clusterset
    if (node_id == 1) {
      const auto http_port =
          original_clusterset_data.clusters[kFirstReplicaClusterId]
              .nodes[0]
              .http_port;
      set_mock_metadata(view_id, /*this_cluster_id*/ 1,
                        /*target_cluster_id*/ kPrimaryClusterId, http_port,
                        clusterset_data_, router_options);
    }

    // wait for the Router to refresh the metadata
    EXPECT_TRUE(wait_for_transaction_count_increase(
        clusterset_data_.clusters[kPrimaryClusterId].nodes[0].http_port, 2));

    // check that the list of the nodes is reflected in the state file
    EXPECT_EQ(node_id, clusterset_data_.get_all_nodes_classic_ports().size());
    check_state_file(router_state_file, mysqlrouter::ClusterType::GR_CS,
                     clusterset_data_.uuid,
                     clusterset_data_.get_all_nodes_classic_ports(), view_id);
  }

  SCOPED_TRACE("// The connections via the Router should be possible again");
  make_new_connection_ok(router_port_rw,
                         clusterset_data_.clusters[kPrimaryClusterId]
                             .nodes[kRWNodeId]
                             .classic_port);

  make_new_connection_ok(router_port_ro,
                         clusterset_data_.clusters[kPrimaryClusterId]
                             .nodes[kRONodeId + 1]
                             .classic_port);
}

/**
 * @test Check that the Router works correctly when can't access some metadata
 * servers.
 * [@FR10]
 * [@TS_R11_5]
 */
TEST_F(ClusterSetTest, SomeMetadataServerUnaccessible) {
  uint64_t view_id = 1;
  const std::string router_options = R"({"targetCluster" : "primary"})";

  create_clusterset(view_id, /*target_cluster_id*/ 0,
                    /*primary_cluster_id*/ 0, "metadata_clusterset.js",
                    router_options);

  SCOPED_TRACE("// Launch Router with target_cluster=primary");
  /*auto &router =*/launch_router();

  auto rw_con1 = make_new_connection_ok(
      router_port_rw, clusterset_data_.clusters[kPrimaryClusterId]
                          .nodes[kRWNodeId]
                          .classic_port);

  auto ro_con1 = make_new_connection_ok(
      router_port_ro, clusterset_data_.clusters[kPrimaryClusterId]
                          .nodes[kRONodeId]
                          .classic_port);

  SCOPED_TRACE("// Make the first Replica Cluster nodes unaccessible");
  for (unsigned node_id = 0; node_id < 3; ++node_id) {
    clusterset_data_.clusters[kFirstReplicaClusterId]
        .nodes[node_id]
        .process->kill();
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[kPrimaryClusterId].nodes[0].http_port, 2));

  SCOPED_TRACE("// Bump up the view_id on the second Replica (remove First)");
  view_id++;
  for (const auto &node :
       clusterset_data_.clusters[kSecondReplicaClusterId].nodes) {
    const auto http_port = node.http_port;
    set_mock_metadata(view_id, /*this_cluster_id*/ kSecondReplicaClusterId,
                      /*target_cluster_id*/ kPrimaryClusterId, http_port,
                      clusterset_data_, router_options);
  }

  EXPECT_TRUE(wait_for_transaction_count_increase(
      clusterset_data_.clusters[kSecondReplicaClusterId].nodes[0].http_port,
      2));

  SCOPED_TRACE(
      "// The existing connections should still be alive, new ones should be "
      "possible");
  verify_existing_connection_ok(rw_con1.get());
  verify_existing_connection_ok(ro_con1.get());
  make_new_connection_ok(router_port_rw,
                         clusterset_data_.clusters[kPrimaryClusterId]
                             .nodes[kRWNodeId]
                             .classic_port);
  make_new_connection_ok(router_port_ro,
                         clusterset_data_.clusters[kPrimaryClusterId]
                             .nodes[kRONodeId + 1]
                             .classic_port);
}

int main(int argc, char *argv[]) {
  init_windows_sockets();
  g_origin_path = Path(argv[0]).dirname();
  ProcessManager::set_origin(g_origin_path);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
