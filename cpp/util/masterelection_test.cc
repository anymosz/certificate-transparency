#include "util/masterelection.h"

#include <atomic>
#include <event2/thread.h>
#include <map>
#include <string>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "base/notification.h"
#include "util/fake_etcd.h"
#include "util/etcd.h"
#include "util/periodic_closure.h"
#include "util/testing.h"


namespace cert_trans {

using cert_trans::Notification;
using std::map;
using std::placeholders::_1;
using std::placeholders::_2;
using std::string;
using std::to_string;
using std::vector;
using testing::AllOf;
using testing::Contains;
using testing::InvokeArgument;
using testing::Pair;
using testing::_;
using util::Status;


const char kProposalDir[] = "/master/";

DECLARE_int32(master_keepalive_interval_seconds);


// Target for kicking events.
void DoNothing() {
}


// Simple helper class, represents a thread of interest in participating in
// an election.
struct Participant {
  // Constructs a new MasterElection object, and immediately starts
  // participating in the election.
  Participant(const string& dir, const string& id,
              const std::shared_ptr<libevent::Base>& base,
              FakeEtcdClient& client)
      : base_(base),
        // client_(base_, "localhost", 4001),
        client_(client),
        election_(new MasterElection(base_, &client_, dir, id)),
        dir_(dir),
        id_(id),
        mastership_count_(0) {
    EXPECT_FALSE(election_->IsMaster()) << id_;
  }


  void StartElection() {
    election_->StartElection();
  }


  void StopElection() {
    VLOG(1) << id_ << " about to StopElection().";
    election_->StopElection();
    VLOG(1) << id_ << " completed StopElection().";
    EXPECT_FALSE(election_->IsMaster()) << id_;
  }


  // Wait to become the boss!
  void ElectLikeABoss(
      const std::function<void(void)>& done = std::function<void(void)>()) {
    StartElection();
    VLOG(1) << id_ << " about to WaitToBecomeMaster().";
    election_->WaitToBecomeMaster();
    EXPECT_TRUE(election_->IsMaster()) << id_;
    ++mastership_count_;
    VLOG(1) << id_ << " completed WaitToBecomeMaster().";
    if (done) {
      done();
    }
  }


  bool IsMaster() {
    return election_->IsMaster();
  }


  void ElectionMania(
      int num_rounds,
      const vector<std::unique_ptr<Participant>>* all_participants) {
    notification_.reset(new Notification);
    mania_thread_.reset(
        new std::thread([this, num_rounds, all_participants]() {
          for (int round(0); round < num_rounds; ++round) {
            VLOG(1) << id_ << " starting round " << round;
            ElectLikeABoss();

            int num_masters(0);
            for (const std::unique_ptr<Participant>& participant :
                 *all_participants) {
              if (participant->election_->IsMaster()) {
                ++num_masters;
              }
            }
            // There /could/ be no masters if an update happened after we came
            // out of WaitToBecomeMaster, it's unlikely but possible.
            // There definitely shouldn't be > 1 master EVER, though.
            CHECK_LE(num_masters, 1) << "From the PoV of " << id_;
            StopElection();
            VLOG(1) << id_ << " finished round " << round;
            // election_.reset(new MasterElection(base_, &client_, dir_, id_));
          }
          VLOG(1) << id_ << " Mania over!";
          notification_->Notify();
        }));
  }


  void WaitForManiaToEnd() {
    CHECK(notification_);
    notification_->WaitForNotification();
    mania_thread_->join();
    mania_thread_.reset();
  }


  const std::shared_ptr<libevent::Base>& base_;
  // EtcdClient client_;
  FakeEtcdClient& client_;
  std::unique_ptr<MasterElection> election_;
  std::unique_ptr<Notification> notification_;
  std::unique_ptr<std::thread> mania_thread_;
  const string dir_;
  const string id_;
  std::atomic<int> mastership_count_;
};


class ElectionTest : public ::testing::Test {
 public:
  ElectionTest()
      : base_(std::make_shared<libevent::Base>()),
        running_(false),
        client_(base_) {
  }


  void SetUp() {
    running_.store(true);
    event_pump_.reset(
        new std::thread(std::bind(&ElectionTest::EventPump, this)));
  }


  void TearDown() {
    running_.store(false);
    base_->Add(std::bind(&DoNothing));
    event_pump_->join();
    event_pump_.reset();
  }


 protected:
  void EventPump() {
    // Prime the pump with a pending event some way out in the future,
    // otherwise we're racing the main thread to get an event in before calling
    // DispatchOnce() (which will CHECK fail if there's nothing to do.)
    libevent::Event event(*base_, -1, 0, std::bind(&DoNothing));
    event.Add(std::chrono::seconds(60));
    while (running_.load()) {
      base_->DispatchOnce();
    }
  }


  void KillProposalRefresh(Participant* p) {
    p->election_->proposal_refresh_callback_.reset();
  }


  std::shared_ptr<libevent::Base> base_;
  std::atomic<bool> running_;
  FakeEtcdClient client_;
  std::unique_ptr<std::thread> event_pump_;
};


typedef class ElectionTest ElectionDeathTest;


TEST_F(ElectionTest, SingleInstanceBecomesMaster) {
  Participant one(kProposalDir, "1", base_, client_);
  EXPECT_FALSE(one.IsMaster());

  one.ElectLikeABoss();
  EXPECT_TRUE(one.IsMaster());

  one.StopElection();
  EXPECT_FALSE(one.IsMaster());
}


TEST_F(ElectionTest, MultiInstanceElection) {
  Participant one(kProposalDir, "1", base_, client_);
  one.ElectLikeABoss();
  EXPECT_TRUE(one.IsMaster());

  Participant two(kProposalDir, "2", base_, client_);
  two.StartElection();
  sleep(1);
  EXPECT_FALSE(two.IsMaster());

  Participant three(kProposalDir, "3", base_, client_);
  three.StartElection();
  sleep(1);
  EXPECT_FALSE(three.IsMaster());

  EXPECT_TRUE(one.IsMaster());

  one.StopElection();
  EXPECT_FALSE(one.IsMaster());

  sleep(2);
  EXPECT_FALSE(one.IsMaster());
  EXPECT_TRUE(two.IsMaster());
  EXPECT_FALSE(three.IsMaster());

  two.StopElection();
  EXPECT_FALSE(two.IsMaster());

  sleep(2);
  EXPECT_FALSE(one.IsMaster());
  EXPECT_FALSE(two.IsMaster());
  EXPECT_TRUE(three.IsMaster());

  three.StopElection();
  EXPECT_FALSE(three.IsMaster());

  sleep(2);
  EXPECT_FALSE(one.IsMaster());
  EXPECT_FALSE(two.IsMaster());
  EXPECT_FALSE(three.IsMaster());
}


TEST_F(ElectionTest, RejoinElection) {
  Participant one(kProposalDir, "1", base_, client_);
  EXPECT_FALSE(one.IsMaster());

  one.ElectLikeABoss();
  EXPECT_TRUE(one.IsMaster());

  one.StopElection();
  EXPECT_FALSE(one.IsMaster());

  // Join in again:
  one.ElectLikeABoss();
  EXPECT_TRUE(one.IsMaster());

  one.StopElection();
  EXPECT_FALSE(one.IsMaster());
}


TEST_F(ElectionTest, ElectionMania) {
  const int kNumRounds(20);
  const int kNumParticipants(20);
  std::vector<std::unique_ptr<Participant>> participants;
  participants.reserve(kNumParticipants);
  for (int i = 0; i < kNumParticipants; ++i) {
    participants.emplace_back(
        new Participant(kProposalDir, to_string(i), base_, client_));
  };

  for (int i = 0; i < kNumParticipants; ++i) {
    participants[i]->ElectionMania(kNumRounds, &participants);
  }

  for (int i = 0; i < kNumParticipants; ++i) {
    LOG(INFO) << i << " became master " << participants[i]->mastership_count_
              << " times.";
    participants[i]->WaitForManiaToEnd();
  }
}


}  // namespace cert_trans


int main(int argc, char** argv) {
  cert_trans::test::InitTesting(argv[0], &argc, &argv, true);
  return RUN_ALL_TESTS();
}
