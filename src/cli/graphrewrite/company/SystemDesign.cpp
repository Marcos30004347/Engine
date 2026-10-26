#include "Company.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <set>

namespace company {
namespace {
template<class T> const T& find(const std::vector<T>& nodes,const std::string& id) {
  auto it=std::find_if(nodes.begin(),nodes.end(),[&](const T& n){return n.id==id;});
  if(it==nodes.end()) throw std::invalid_argument("Unknown design node: "+id);
  return *it;
}
template<class T> T& find(std::vector<T>& nodes,const std::string& id) {
  auto it=std::find_if(nodes.begin(),nodes.end(),[&](const T& n){return n.id==id;});
  if(it==nodes.end()) throw std::invalid_argument("Unknown design node: "+id);
  return *it;
}
const char* computeNames[]={"Lambda","EC2 multi-AZ + ALB","EC2 single-AZ + ALB","ECS Fargate + ALB"};
const char* databaseNames[]={"DynamoDB","RDS multi-AZ","RDS public single-AZ","Aurora PostgreSQL"};
std::vector<std::string> split(const std::string& input) {
  std::istringstream in(input); std::vector<std::string> parts; std::string part;
  while(std::getline(in,part,':')) parts.push_back(part);
  return parts;
}
}
void Game::setupDesign() {
  s.design.compute={{"A","Shared application server"}};
  s.design.databases={{"D","Shared application database"}};
  s.design.data={{"B1","Members and entitlements","keyed","D"},
    {"B2","Catalogue","keyed","D"},{"B3","Subscriptions and invoices","relational","D"},
    {"B4","Viewing history","keyed","D"}};
  s.design.apis={{"U1","Accounts","bursty authentication and entitlement lookups","A",{"B1"}},
    {"U2","Catalogue","bursty catalogue reads by identifier","A",{"B2"}},
    {"U3","Payments","invoice reconciliation and relational reporting","A",{"B1","B3"}},
    {"U4",s.business==Business::Game?"Matchmaking":"Playback","short session and entitlement requests","A",{"B1","B2","B4"}},
    {"U5","Recommendations","history queries by member","A",{"B2","B4"}}};
}
std::string Game::implementationDecision(int task) const {
  if(task==0) return "design:database:"+find(s.design.data,"B2").database;
  if(task==2) return "design:compute:"+find(s.design.apis,"U3").compute;
  return s.tasks.at(task).decision;
}
void Game::chargeRewrite(int cost) {
  if(s.phase==Phase::Launch || s.phase==Phase::Maintenance) {
    spend(cost); ++s.architectureChanges; s.lastMigrationWeek=s.week;
  }
}
bool Game::designReady() const {
  for(const auto& api:s.design.apis) {
    if(find(s.design.compute,api.compute).service<0) return false;
    for(const auto& id:api.data) if(find(s.design.databases,find(s.design.data,id).database).service<0) return false;
  }
  return true;
}
std::vector<ArchitectureNode> Game::designNodes() const {
  std::vector<ArchitectureNode> nodes{{"G","design:gateway","API Gateway","routes each API to its selected compute node",40}};
  for(const auto& c:s.design.compute) {
    std::string routes; int count=0;
    for(const auto& api:s.design.apis) if(api.compute==c.id) { routes+=api.id+" "+api.name+"; "; ++count; }
    const int base[]={80,1450,480,850},rate[]={12,6,9,8};
    const int bill=c.service<0?0:base[c.service]+s.customers*rate[c.service]*count/(100*static_cast<int>(s.design.apis.size()));
    nodes.push_back({c.id,"design:compute:"+c.id,c.service<0?"Server (undecided)":computeNames[c.service],
      routes.empty()?"idle; still billed until retired":routes,bill});
  }
  for(const auto& db:s.design.databases) {
    std::string datasets; int count=0;
    for(const auto& data:s.design.data) if(data.database==db.id) { datasets+=data.id+" "+data.name+"; "; ++count; }
    const int base[]={90,950,280,700},rate[]={8,3,4,4};
    const int bill=db.service<0?0:base[db.service]+s.customers*rate[db.service]*count/(100*static_cast<int>(s.design.data.size()))+
      (db.pooled && db.service!=0?120:0);
    nodes.push_back({db.id,"design:database:"+db.id,db.service<0?"Database (undecided)":databaseNames[db.service],
      (datasets.empty()?"idle; still billed until retired":datasets)+(db.pooled?" | pooled connections":" | direct connections"),bill});
  }
  return nodes;
}
std::vector<std::pair<std::string,std::string>> Game::designEdges() const {
  std::set<std::pair<std::string,std::string>> edges;
  for(const auto& api:s.design.apis) {
    edges.insert({"G",api.compute});
    for(const auto& id:api.data) edges.insert({api.compute,find(s.design.data,id).database});
  }
  return {edges.begin(),edges.end()};
}
std::vector<std::string> Game::systemComponents(const std::string& system) const {
  std::vector<std::string> labels;
  auto add=[&](std::string id){if(std::find(labels.begin(),labels.end(),id)==labels.end()) labels.push_back(id);};
  if(system=="1" || system=="2" || system=="3" || system=="4") {
    const auto& api=s.design.apis.at(static_cast<size_t>(system[0]-'1'));
    add("G"); add(api.compute);
    for(const auto& data:api.data) add(find(s.design.data,data).database);
    if(system=="4") { add("E"); add("S"); add("P"); }
    else add("I");
  } else if(system=="5") { for(const auto& node:s.design.compute) add(node.id); add("W"); add("S"); add("P"); add("I"); }
  else if(system=="6") { for(const auto& node:s.design.compute) add(node.id); add("P"); add("I"); add("R"); add("O"); }
  else throw std::invalid_argument("Unknown system");
  return labels;
}
Decision Game::designDecision(const std::string& id) const {
  if(id=="design") return {id,"System design workshop",
    "First decide ownership: which APIs share a server, and which datasets share a database? AWS services can be chosen afterwards.",
    {{"One shared server and database behind API Gateway"},{"Separate payments compute and invoice storage"}}};
  if(id=="design:next") {
    for(const auto& api:s.design.apis) {
      if(find(s.design.compute,api.compute).service<0) return designDecision("design:compute:"+api.compute);
      for(const auto& data:api.data) {
        const auto& db=find(s.design.databases,find(s.design.data,data).database);
        if(db.service<0) return designDecision("design:database:"+db.id);
      }
    }
    return designDecision("design:gateway");
  }
  const auto parts=split(id);
  if(parts.size()<2) throw std::invalid_argument("Invalid design decision");
  const auto action=parts[1];
  Decision d{id,"Architecture desk","",{}};
  if(action=="add-compute" || action=="add-database") {
    d.text="Add an undecided node to the blueprint. Choose its service and assign work afterwards.";
    d.options={{"Create node"}}; return d;
  }
  if(action=="gateway") return {id,"API Gateway","Routes follow API ownership. Select an API to move its implementation while preserving its data dependencies.",{{"Keep routing"}}};
  if(parts.size()!=3) throw std::invalid_argument("Missing design node");
  const auto target=parts[2];
  if(action=="compute") {
    const auto& c=find(s.design.compute,target);
    d.text="["+c.id+"] "+c.name+". This changes only the APIs assigned here. Live rewrites cost $900.";
    for(const auto& api:s.design.apis) if(api.compute==target) d.text+="\n"+api.id+" "+api.name+": "+api.usage;
    d.options={{"Lambda | $80 base/month"},{"EC2 multi-AZ + ALB | $1,450 base/month"},
      {"EC2 single-AZ + ALB | $480 base/month"},{"Fargate + ALB | $850 base/month"}};
  } else if(action=="database") {
    const auto& db=find(s.design.databases,target);
    d.text="["+db.id+"] "+db.name+". Change the engine for all datasets here with a verified conversion. Live changes cost $1,200.";
    for(const auto& data:s.design.data) if(data.database==target) d.text+="\n"+data.id+" "+data.name+": "+data.access+" access";
    d.options={{"DynamoDB | known key access patterns; $90 base/month"},{"RDS multi-AZ | SQL; $950 base/month"},
      {"Public single-AZ RDS, shared credentials | $280 base/month"},{"Aurora PostgreSQL | SQL; $700 base/month"}};
  } else if(action=="route") {
    const auto& api=find(s.design.apis,target);
    d.text="["+api.id+"] "+api.name+": "+api.usage+". Move this API; keep its dataset dependencies. Live cutover costs $900.";
    for(const auto& c:s.design.compute) d.options.push_back({c.id+" | "+(c.service<0?"server undecided":computeNames[c.service])});
    d.options.push_back({"Extract into a new dedicated Lambda"});
    d.options.push_back({"Extract into a new EC2 multi-AZ service"});
    d.options.push_back({"Extract into a new Fargate service"});
  } else if(action=="data") {
    const auto& data=find(s.design.data,target);
    d.text="["+data.id+"] "+data.name+" uses "+data.access+" access. Rehome only this dataset; all of its API consumers follow the cutover.";
    for(const auto& db:s.design.databases) {
      d.options.push_back({db.id+" | copy, adapt queries, verify and switch | $1,200 live"});
      d.options.push_back({db.id+" | switch endpoints without a copy/reconciliation rehearsal | $250 live"});
    }
  } else if(action=="pool") {
    find(s.design.databases,target); d.text="Connection policy for "+target+". Relational pooling adds $120/month; policy changes cost $300 live.";
    d.options={{"Use RDS Proxy / pooled connections"},{"Connect directly"}};
  } else if(action=="verify") {
    find(s.design.data,target); d.text="Reconcile copied records and validate access patterns for "+target+". Repair costs $600 live.";
    d.options={{"Reconcile and validate the dataset"}};
  } else if(action=="retire") {
    bool exists=false;
    for(const auto& c:s.design.compute) exists|=c.id==target;
    for(const auto& db:s.design.databases) exists|=db.id==target;
    if(!exists) throw std::invalid_argument("Unknown node");
    d.text="Retire "+target+" only if no API or dataset still depends on it. This ends recurring billing; $300 live."; d.options={{"Retire unused node"}};
  } else throw std::invalid_argument("Unknown design operation: "+action);
  return d;
}
std::string Game::chooseDesign(const Decision& d,size_t option) {
  const auto current=designDecision(d.id);
  if(option>=current.options.size()) throw std::invalid_argument("Design choice out of range");
  const auto beforeFacts=designFacts(); const int before=cloudCost(),cash=s.cash;
  const auto result=applyDesign(current,option);
  if(designFacts()!=beforeFacts) {
    s.designHistory.push_back({s.week,before,cloudCost(),cash-s.cash,(current.id=="design"?"System ownership":current.id.substr(7))+" — "+current.options.at(option).label});
    if(s.designHistory.size()>12) s.designHistory.erase(s.designHistory.begin());
  }
  return result;
}
std::string Game::applyDesign(const Decision& d,size_t option) {
  const auto valid=designDecision(d.id);
  if(option>=valid.options.size()) throw std::invalid_argument("Design choice out of range");
  auto& graph=s.design;
  if(d.id=="design") {
    if(graph.planned) return "The ownership plan is already recorded. Move individual APIs or datasets at the design desk.";
    if(option==1) {
      graph.compute.push_back({"C"+std::to_string(graph.nextCompute++),"Payments service"});
      graph.databases.push_back({"D"+std::to_string(graph.nextDatabase++),"Invoice store"});
      find(graph.apis,"U3").compute=graph.compute.back().id;
      find(graph.data,"B3").database=graph.databases.back().id;
    }
    graph.planned=true;
    return "The team records API ownership and data boundaries. The server and database boxes can now be refined independently into AWS services.";
  }
  const auto p=split(d.id); const auto action=p[1]; const auto id=p.size()>2?p[2]:"";
  if(action=="add-compute" || action=="add-database") {
    if(graph.compute.size()+graph.databases.size()>=10) return "Ten infrastructure nodes is the demo limit. Retire an unused node first.";
    if(action=="add-compute") { const auto key="C"+std::to_string(graph.nextCompute++); graph.compute.push_back({key,"Application service"}); return "Created ["+key+"] Server (undecided)."; }
    const auto key="D"+std::to_string(graph.nextDatabase++); graph.databases.push_back({key,"Application data store"}); return "Created ["+key+"] Database (undecided).";
  }
  if(action=="compute") {
    auto& node=find(graph.compute,id);
    if(node.service==static_cast<int>(option)) return "That compute node is already configured this way.";
    chargeRewrite(900); node.service=static_cast<int>(option); if(id=="A") s.api=node.service;
  } else if(action=="database") {
    auto& node=find(graph.databases,id);
    if(node.service==static_cast<int>(option)) return "That database is already configured this way.";
    chargeRewrite(1200); node.service=static_cast<int>(option); if(id=="D") s.database=node.service;
    for(auto& data:graph.data) if(data.database==id) { data.verified=true; data.modeled=true; }
  } else if(action=="route") {
    auto& api=find(graph.apis,id);
    if(option<graph.compute.size()) {
      if(api.compute==graph.compute[option].id) return "That API already runs there.";
      chargeRewrite(900); api.compute=graph.compute[option].id;
    } else {
      if(graph.compute.size()+graph.databases.size()>=10) return "Retire an unused infrastructure node before extracting another API.";
      const int services[]={0,1,3}; const int service=services[option-graph.compute.size()];
      const auto key="C"+std::to_string(graph.nextCompute++);
      chargeRewrite(900); graph.compute.push_back({key,api.name+" service",service}); api.compute=key;
    }
  } else if(action=="data") {
    auto& data=find(graph.data,id); const auto& db=graph.databases[option/2];
    if(data.database==db.id) return "The dataset already lives there. Use reconcile to repair its current migration.";
    if(db.service<0) return "Choose the destination database service before copying data into it.";
    const bool careful=option%2==0; chargeRewrite(careful?1200:250);
    data.database=db.id; data.verified=careful; data.modeled=careful;
  } else if(action=="pool") {
    auto& db=find(graph.databases,id); const bool pooled=option==0;
    if(db.service==0 || db.service<0) return "Relational connection pooling applies to RDS and Aurora; choose a relational engine first.";
    if(db.pooled==pooled) return "The connection policy is unchanged.";
    chargeRewrite(300); db.pooled=pooled;
  } else if(action=="verify") {
    auto& data=find(graph.data,id); if(data.verified && data.modeled) return "The dataset already has a verified migration and access model.";
    chargeRewrite(600); data.verified=true; data.modeled=true;
  } else if(action=="retire") {
    for(const auto& api:graph.apis) if(api.compute==id) return "Move the APIs off this server before retiring it.";
    for(const auto& data:graph.data) if(data.database==id) return "Migrate the datasets out before retiring this database.";
    // A and D remain stable anchors for task-level implementation decisions.
    if(id=="A" || id=="D") {
      if((id=="A" && find(graph.compute,id).service<0) || (id=="D" && find(graph.databases,id).service<0)) return "That original placeholder is already undeployed.";
      chargeRewrite(300);
      if(id=="A") find(graph.compute,id).service=-1;
      else find(graph.databases,id).service=-1;
      return "The unused original node is undeployed and no longer billed. Its undecided placeholder remains available for future work.";
    }
    chargeRewrite(300);
    graph.compute.erase(std::remove_if(graph.compute.begin(),graph.compute.end(),[&](const ComputeNode& n){return n.id==id;}),graph.compute.end());
    graph.databases.erase(std::remove_if(graph.databases.begin(),graph.databases.end(),[&](const DatabaseNode& n){return n.id==id;}),graph.databases.end());
  }
  return "The graph rewrite is recorded. API ownership, backend connections and the recurring bill now follow the new topology.";
}
std::string Game::designFacts() const {
  std::ostringstream out;
  if(!designReady()) out << "Company configured designunresolved\n";
  if(!s.design.planned) out << "Company configured designpending\n";
  for(const auto& c:s.design.compute) out << c.id << " configured " << (c.service<0?"undecided":c.service==0?"lambda":c.service==2?"singlezone":"server") << '\n';
  for(const auto& db:s.design.databases) {
    out << db.id << " configured " << (db.service<0?"undecided":db.service==0?"dynamo":"sql") << '\n';
    if(db.service==2) out << db.id << " configured publicdata\n";
    out << db.id << " configured " << (db.pooled?"pooled":"direct") << '\n';
  }
  for(const auto& c:s.design.compute) if(c.service>=0 && std::none_of(s.design.apis.begin(),s.design.apis.end(),[&](const ApiRoute& api){return api.compute==c.id;})) out << c.id << " configured idle\n";
  for(const auto& db:s.design.databases) if(db.service>=0 && std::none_of(s.design.data.begin(),s.design.data.end(),[&](const DataSet& data){return data.database==db.id;})) out << db.id << " configured idle\n";
  for(const auto& data:s.design.data) {
    out << data.id << " stored in " << data.database << '\n';
    out << data.id << " configured " << data.access << '\n';
    if(!data.verified) out << data.id << " configured unverified\n";
    if(!data.modeled) out << data.id << " configured unmodeled\n";
  }
  for(const auto& api:s.design.apis) {
    out << "Company exposes " << api.id << '\n' << api.id << " runs on " << api.compute << '\n';
    for(const auto& data:api.data) out << api.id << " uses " << data << '\n';
  }
  return out.str();
}
}
