#pragma once

#include <array>
#include <map>
#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include <utility>

namespace company {
enum class Phase { Development, Launch, Maintenance };
enum class Business { Video, Game };
enum class Difficulty { Easy, Normal, Hard };
struct RuleResult { std::map<std::string,std::string> bindings; std::string id, text, action; int cost=0, days=0, trust=0, perCustomer=0; bool breach=false; };
struct ArchitectureNode { std::string label, decision, service, settings; int monthlyCost; };
struct ComputeNode { std::string id, name; int service = -1; };
struct DatabaseNode { std::string id, name; int service = -1; bool pooled = false; };
struct DataSet { std::string id, name, access, database; bool verified = true, modeled = true; };
struct ApiRoute { std::string id, name, usage, compute; std::vector<std::string> data; };
struct DesignChange { int week, before, after, cost; std::string summary; };
struct SystemDesign {
  bool planned = false;
  int nextCompute = 2, nextDatabase = 2;
  std::vector<ComputeNode> compute;
  std::vector<DatabaseNode> databases;
  std::vector<DataSet> data;
  std::vector<ApiRoute> apis;
};
struct ArchitectureImpact { std::string text; int cost = 0, days = 0, trust = 0; bool breach = false; };
struct Person {
  std::string name, experience;
  int age = 0; // years for humans, months since activation for agents
  int tier = -1;
  int technical = 0, context = 0, monthlyCost = 0;
  int morale = 78, nextReviewWeek = 52;
  bool human() const { return tier < 0; }
  std::string description() const;
};
struct Option { std::string label; };
struct Decision {
  std::string id, speaker, text;
  std::vector<Option> options;
};
struct Proposal { std::string speaker, text; size_t option = 0; };
struct ProjectTask {
  std::string title, decision;
  std::vector<int> dependencies;
  int progress = 0;
  bool contextual = false, designMade = false;
};
struct Assignment { int task; std::string worker; bool needsAnswer = false; };
struct Ledger {
  int revenue = 0, humans = 0, agents = 0, cloud = 0, licensing = 0, incidents = 0;
  int costs() const { return humans + agents + cloud + licensing + incidents; }
  int profit() const { return revenue - costs(); }
};
struct State {
  SystemDesign design;
  std::vector<DesignChange> designHistory;
  uint32_t storySeed = 1;
  int week = 0, cash = 85000, customers = 0, trust = 75, quality = 40, progress = 0;
  Phase phase = Phase::Development;
  Business business = Business::Video;
  bool workPrepared = false, workResolved = false;
  int developmentWeeks = 18, developmentStart = 0;
  int projectScope = 0;
  int monthlyRevenue = 0, monthlyCosts = 0;
  std::string lastMonthlyReport;
  std::array<Person, 7> applicants;
  std::vector<ProjectTask> tasks;
  std::vector<Assignment> assignments;
  int storage = 0, scaling = 0, database = 0, monitoring = 0;
  int architectureChanges = 0, lastMigrationWeek = -1, scenarioCursor = 0;
  int rivalCustomers = 0;
  std::string rivalName = "Silverline", intelDecision = "api";
  int intelOption = 0;
  int api = 0, delivery = 0, recovery = 0, encoding = 0;
  bool scopedIdentity = true, reviewed = true, testedRecovery = false;
  bool weekOpen = false, incidentThisWeek = false, launchProcessed = false;
  int incidentCost = 0, lostDays = 0, breaches = 0, outages = 0, questions = 0;
  int peakCustomers = 0, resignations = 0, founderEquity = 100, investment = 0;
  int lastPitchWeek = -4, rounds = 0, marketSize = 5;
  int committedHumans = 0, committedAgents = 0;
  std::array<int, 7> availableAfter{};
  std::array<int, 7> prices{6200, 3000, 450, 2200, 8500, 1300, 11000};
  std::string companyName, lastIncident = "the idea scribbled on a cinema ticket", raiseName;
  std::vector<Person> team;
  std::vector<std::string> committedNames;
  Ledger ledger;
};

// Seed + identical decisions produce identical state and prose. No network or LLM required.
class Game {
public:
  static constexpr int campaignWeeks = 52;
  explicit Game(uint32_t seed, Business business = Business::Video, Difficulty difficulty = Difficulty::Normal);
  Proposal proposal(const Decision& decision) const;
  void refreshCandidates();
  std::string architectureReport() const;
  std::string designGraph(const std::string& api = "") const;
  std::vector<ArchitectureNode> designNodes() const;
  std::vector<std::pair<std::string,std::string>> designEdges() const;
  std::string designFacts() const;
  bool designReady() const;
  Decision designDecision(const std::string& id) const;
  std::string chooseDesign(const Decision& d, size_t option);
  std::vector<std::string> systemComponents(const std::string& system) const;
  std::string systemGraph(const std::string& system = "") const;
  std::string graphFacts() const;
  RuleResult rule(const std::string& domain, const std::string& facts = "") const;
  std::string hiringAdvice() const;
  const State& state() const { return s; }
  std::string introduction() const;
  std::string phaseName() const;
  std::string dashboard() const;
  std::string finances() const;
  std::vector<ArchitectureNode> architectureNodes() const;
  std::vector<std::pair<std::string,std::string>> architectureEdges() const;
  std::string architecture(bool compact = false) const;
  std::string nodeDecision(const std::string& label) const;
  ArchitectureImpact assessArchitecture(const std::string& scenario) const;
  std::string exercise(const std::string& label);
  std::string rivalReport() const;
  std::string localize(std::string text) const;
  Decision decision(const std::string& id) const;
  std::string choose(const Decision& d, size_t option);
  std::string beginWeek();
  Decision chapter() const;
  std::vector<Decision> questions() const;
  std::string event();
  std::string closeWeek();
  std::string ending() const;
  bool bankrupt() const { return s.cash <= 0; }
  int cloudCost() const; // monthly equivalent; game month = four weeks
  Ledger forecast() const; // next week's recurring costs
  Person candidate(int index) const;
  std::vector<std::string> planners() const;
  std::vector<int> availableTasks() const;
  std::string taskBoard(bool monthly = false) const;
  std::string prepareWork();
  std::string resolveWork();
  bool readyToLaunch() const;
  std::string monthlyReview() const;
private:
  State s;
  std::mt19937 rng;
  int roll(int bound);
  void normalize();
  void spend(int amount);
  void commitPay(const Person& person);
  std::string speaker() const;
  std::string architectureEvent();
  void updateIntel();
  Person worker(const std::string& name) const;
  void setupProject();
  void setupDesign();
  std::string applyDesign(const Decision& d, size_t option);
  std::string implementationDecision(int task) const;
  void chargeRewrite(int cost);
};
int runCli(int argc, char** argv);
}
