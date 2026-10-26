#include "Company.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace company {
namespace {
const char* edge[] = {"CloudFront", "Direct public S3", "Application proxy"};
const char* storage[] = {"S3 Standard", "S3 Intelligent-Tiering", "S3 Standard-IA", "S3 Glacier Deep Archive", "EBS + EC2 origin"};
const char* policies[] = {"bounded demand scaling", "fixed small capacity", "aggressive high limits", "peak capacity always warm"};
const char* recovery[] = {"one Region", "cross-Region backups", "pilot light", "warm standby", "active/active"};
std::string scenarioFor(const std::string& label) {
  if (label == "S") return "archive";
  if (label == "D") return "database";
  if (label == "I") return "leak";
  if (label == "R") return "restore";
  if (label == "W") return "jobs";
  if (label == "O") return "bill";
  return "surge";
}
}
std::vector<ArchitectureNode> Game::architectureNodes() const {
  const int edgeBase[] = {180,40,420}, edgeRate[] = {65,120,185};
  const int storeBase[] = {220,160,95,25,620}, storeRate[] = {12,9,6,1,8};
  const int scaleBase[] = {120,0,180,3200}, recoverBase[] = {0,250,850,2300,5200};
  const int workerBase[] = {s.business == Business::Game ? 700 : 350,80,s.business == Business::Game ? 900 : 1650};
  const int monitorBase[] = {140,35,0};
  const std::string worker = s.encoding == 0 ? (s.business == Business::Game ? "Fargate session tasks" : "SQS + MediaConvert") :
    s.encoding == 1 ? "standard Lambda jobs" : "EC2 worker fleet";
  auto result = designNodes();
  const std::vector<ArchitectureNode> infrastructure = {
    {"E","delivery",edge[s.delivery],s.delivery == 0 ? (s.storage == 4 ? "signed viewers; EC2 custom origin" : "signed viewers; private S3 OAC origin") :
      s.delivery == 1 ? "anonymous object access" : "media travels through application servers",edgeBase[s.delivery]+s.customers*edgeRate[s.delivery]/100},
    {"S","storage",storage[s.storage],s.storage == 4 ? "block storage; one-AZ EC2 origin" : s.storage == 1 ? "no optional archive tiers; private unless E is public" :
      "live catalogue; access controlled by E",storeBase[s.storage]+s.customers*storeRate[s.storage]/100},
    {"W","encoding",worker,s.business == Business::Game ? "persistent multiplayer sessions" : "forty-minute video encodes",workerBase[s.encoding]},
    {"P","scaling",policies[s.scaling],"shared concurrency and replica limits across compute nodes",scaleBase[s.scaling]},
    {"I","identity","IAM",s.scopedIdentity ? "scoped roles; short-lived credentials" : "shared/broad credentials",0},
    {"R","recovery",recovery[s.recovery],s.testedRecovery ? "restore + routing rehearsed" : "restore + routing untested",recoverBase[s.recovery]},
    {"O","monitoring","CloudWatch / Budgets",s.monitoring == 0 ? "redacted logs; service + billing alerts" :
      s.monitoring == 1 ? "full requests incl. credentials; long retention" : "minimal logs; no alerts",monitorBase[s.monitoring]}
  };
  result.insert(result.end(), infrastructure.begin(), infrastructure.end());
  return result;
}
std::vector<std::pair<std::string,std::string>> Game::architectureEdges() const {
  auto edges=designEdges();
  edges.insert(edges.end(),{{"E","S"},{"W","S"},{"P","W"}});
  for(const auto& c:s.design.compute) {
    edges.push_back({"I",c.id}); edges.push_back({"P",c.id}); edges.push_back({"O",c.id});
  }
  for(const auto& db:s.design.databases) edges.push_back({"R",db.id});
  // Long jobs are a separate service reached by the playback/session API.
  edges.push_back({s.design.apis.at(3).compute,"W"});
  return edges;
}
int Game::cloudCost() const {
  int cost = 0;
  for (const auto& node : architectureNodes()) cost += node.monthlyCost;
  return cost;
}
std::string Game::architecture(bool compact) const {
  std::string text=designGraph();
  if(!compact) {
    for(const auto& n:architectureNodes()) text += "["+n.label+"] "+n.service+" | "+n.settings+" | $"+std::to_string(n.monthlyCost)+"/month\n";
    text += "\n"+rivalReport();
  }
  return text;
}
std::string Game::nodeDecision(const std::string& label) const {
  std::string upper = label;
  for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  for (const auto& node : architectureNodes()) if (node.label == upper) return node.decision;
  throw std::invalid_argument("Unknown architecture label: " + label);
}
ArchitectureImpact Game::assessArchitecture(const std::string& scenario) const {
  const auto effect = rule(scenario);
  if (effect.id.empty()) throw std::invalid_argument("Unknown architecture scenario: " + scenario);
  return {effect.text,effect.cost + s.customers * effect.perCustomer / 100,effect.days,effect.trust,effect.breach};
}
std::string Game::exercise(const std::string& label) {
  nodeDecision(label); // validate before spending
  std::string upper = label;
  for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  spend(200);
  const auto result = assessArchitecture(upper=="G" || upper.size()>1 ? "topology" : scenarioFor(upper));
  return "TABLETOP EXERCISE ($200; no production impact)\n" + result.text +
    "\nEstimated incident: $" + std::to_string(result.cost) + ", " + std::to_string(result.days) + " days of disruption.\n"
    "This uses the same configuration rules as live events; it changes no service settings.";
}
std::string Game::architectureEvent() {
  const char* scenarios[] = {"surge","archive","leak","zone","bill","jobs","database","restore"};
  const std::string scenario = scenarios[s.scenarioCursor++ % 8];
  const auto impact = assessArchitecture(scenario);
  spend(impact.cost); s.lostDays += impact.days; s.trust -= impact.trust;
  if (impact.breach) { ++s.breaches; s.customers = s.customers * 85 / 100; }
  if (impact.days) ++s.outages;
  if (impact.cost || impact.days || impact.breach) { s.incidentThisWeek = true; s.lastIncident = "the " + scenario + " incident on the architecture board"; }
  normalize();
  return "\nTHE ARCHITECTURE MEETS THE WEEK\n" + impact.text + "\n";
}
void Game::updateIntel() {
  if (s.storage >= 2) { s.intelDecision = "storage"; s.intelOption = 0; }
  else if (s.api != 0 && s.customers < 15000) { s.intelDecision = "api"; s.intelOption = 0; }
  else if (s.scaling != 0) { s.intelDecision = "scaling"; s.intelOption = 0; }
  else if (s.database == 2) { s.intelDecision = "database"; s.intelOption = 0; }
  else {
    const char* decisions[] = {"delivery","storage","monitoring","recovery","database","encoding"};
    s.intelDecision = decisions[(s.week / 8) % 6];
    s.intelOption = s.intelDecision == "storage" ? 1 : s.intelDecision == "recovery" ? 2 : 0;
  }
}
std::string Game::rivalReport() const {
  // Compare an actual candidate configuration at the player's current scale, not invented savings.
  Game peer = *this;
  if (s.intelDecision == "api") peer.s.api = s.intelOption;
  else if (s.intelDecision == "delivery") peer.s.delivery = s.intelOption;
  else if (s.intelDecision == "storage") peer.s.storage = s.intelOption;
  else if (s.intelDecision == "scaling") peer.s.scaling = s.intelOption;
  else if (s.intelDecision == "database") peer.s.database = s.intelOption;
  else if (s.intelDecision == "monitoring") peer.s.monitoring = s.intelOption;
  else if (s.intelDecision == "encoding") peer.s.encoding = s.intelOption;
  else if (s.intelDecision == "recovery") peer.s.recovery = s.intelOption;
  peer.s.design.compute.front().service=peer.s.api; peer.s.design.databases.front().service=peer.s.database;
  const int delta = cloudCost() - peer.cloudCost();
  std::ostringstream out;
  out << "RIVAL INTELLIGENCE: " << s.rivalName << "\nA contact shares a design review from " << s.rivalName
      << ": they chose " << decision(s.intelDecision).options[s.intelOption].label << ".\n";
  if (delta > 0) out << "At your current customer count, that change saves $" << delta << "/month on the recurring bill.\n";
  else if (delta < 0) out << "At your scale this costs $" << -delta << " more/month. Their efficiency claim includes avoiding incident and recovery costs.\n";
  else out << "Your recurring cost for this choice already matches theirs. There is no saving to claim here.\n";
  peer.s.api = 0; peer.s.delivery = 0; peer.s.storage = 1; peer.s.database = 0;
  peer.s.scaling = 0; peer.s.monitoring = 0; peer.s.encoding = 0; peer.s.recovery = 2;
  peer.s.design.compute.front().service=peer.s.api; peer.s.design.databases.front().service=peer.s.database;
  out << "Their full reference design at your scale: $" << peer.cloudCost() << "/month; yours: $" << cloudCost() << "/month.\n";
  out << "They separate hot objects from archives, cap scale, keep data private, and use long-lived workers for long-lived jobs.\n"
      << "Their audience is now " << s.rivalCustomers << ". The report is advice for this workload, not a universal best architecture.";
  return out.str();
}
std::string Game::localize(std::string text) const {
  if (s.business != Business::Game) return text;
  // Shared office/finance scenes are reused; service choices and workload effects differ in the model.
  const std::pair<const char*,const char*> vocabulary[] = {
    {"film-club members","beta testers"},{"film club","playtest group"},{"three forgotten films","a cooperative fantasy world"},
    {"forty-minute video encodes","persistent multiplayer sessions"},{"forty-minute encode","long-running match"},
    {"film streaming","fantasy game"},{"Film rights","World content"},{"film rights","world content"},
    {"filmmaker","world designer"},{"director","game designer"},{"subtitles","party chat"},{"projector","game server"},
    {"cinema","arcade"},{"screening","playtest"},{"premiere","expansion launch"},{"catalogue","world and asset library"},
    {"film poster","world map"},{"films","game worlds"},{"film","adventure"},{"playback","session stability"},
    {"watch","play"},{"viewer","player"},{"comedy","questline"},{"popcorn","pizza"}
  };
  for (const auto& pair : vocabulary) {
    size_t pos = 0;
    while ((pos = text.find(pair.first,pos)) != std::string::npos) {
      text.replace(pos,std::char_traits<char>::length(pair.first),pair.second);
      pos += std::char_traits<char>::length(pair.second);
    }
  }
  return text;
}
}

namespace company {
std::string Game::architectureReport() const {
  std::ostringstream out;
  auto nodes=architectureNodes();
  std::stable_sort(nodes.begin(),nodes.end(),[](const ArchitectureNode& a,const ArchitectureNode& b){return a.monthlyCost>b.monthlyCost;});
  out << "ARCHITECTURE REPORT / week " << s.week << " / " << phaseName() << "\n"
      << "Cloud forecast $" << cloudCost() << "/month | weekly accrual $" << forecast().cloud
      << " | " << s.customers << " paying customers\n";
  if(s.customers>0) out << "Cloud cost per paying customer: $" << std::fixed << std::setprecision(2)
    << static_cast<double>(cloudCost())/s.customers << "/month\n";
  else out << "Pre-launch: infrastructure costs are funded from cash; no customer unit cost yet.\n";
  out << "\nCOST BY NODE (shared services counted once)\n";
  int reconciled=0,idleCost=0;
  for(const auto& n:nodes) {
    reconciled+=n.monthlyCost;
    out << '[' << n.label << "] " << n.service << " | $" << n.monthlyCost << "/month\n";
  }
  out << "TOTAL $" << reconciled << "/month (fictional game pricing)\n\nAPI DEPENDENCIES\n";
  for(const auto& api:s.design.apis) {
    out << '[' << api.id << "] " << api.name << ": [G] -> [" << api.compute << "] -> ";
    for(const auto& id:api.data) for(const auto& data:s.design.data) if(data.id==id)
      out << '[' << data.database << ": " << data.id << ' ' << data.name << "] ";
    out << '\n';
  }
  out << "\nIDLE CAPACITY\n";
  for(const auto& node:nodes) {
    bool idle=false;
    for(const auto& c:s.design.compute) if(c.id==node.label && c.service>=0)
      idle=std::none_of(s.design.apis.begin(),s.design.apis.end(),[&](const ApiRoute& api){return api.compute==c.id;});
    for(const auto& db:s.design.databases) if(db.id==node.label && db.service>=0)
      idle=std::none_of(s.design.data.begin(),s.design.data.end(),[&](const DataSet& data){return data.database==db.id;});
    if(idle) { idleCost+=node.monthlyCost; out << '[' << node.label << "] no assigned workload; $" << node.monthlyCost << "/month\n"; }
  }
  out << "Idle infrastructure total: $" << idleCost << "/month\n";
  out << "\nREADINESS\n" << (designReady()?"All active API paths have selected compute and database services.":"Some active compute or database nodes are undecided; launch is blocked.")
      << "\nRecovery: " << (s.testedRecovery?"rehearsed":"not rehearsed") << " | " << recovery[s.recovery] << '\n';
  out << "\nDESIGN REVIEW / observed configuration\n" << rule("review").text << '\n';
  out << "\nRECENT DESIGN CHANGES\n";
  if(s.designHistory.empty()) out << "No topology changes recorded yet.\n";
  for(const auto& change:s.designHistory) out << "Week " << change.week << ": " << change.summary << "\n  Cloud $" << change.before << " -> $" << change.after
    << "/month; one-off $" << change.cost << '\n';
  out << "Reports do not run a load test or change the design. Use a tabletop exercise to rehearse a failure.";
  return out.str();
}
}
