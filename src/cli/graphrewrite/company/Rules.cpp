#include "Company.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/StoryRunner.hpp"
#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>

namespace company {
namespace {
std::string baseRule(std::string id) { return id.substr(0,id.find('#')); }
struct RuleBook {
  graphrewrite::compiler::CompiledProgram program;
  graphrewrite::compiler::UsageValues usage;
  graphrewrite::runtime::Narrator narrator;
  std::map<std::string,RuleResult> effects;
  RuleBook() {
    const char* overridePath=std::getenv("COMPANY_STORY_RULES");
    const std::string path = overridePath ? overridePath : COMPANY_RULES_PATH;
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open company rules: " + path);
    std::ostringstream buffer; buffer << file.rdbuf();
    const auto source = buffer.str();
    graphrewrite::DiagnosticSink diagnostics;
    if (!graphrewrite::compiler::compile(source,path,{},diagnostics,program,usage)) {
      std::string errors;
      for (const auto& d : diagnostics.all()) errors += d.format(path) + "\n";
      throw std::runtime_error(errors);
    }
    std::istringstream lines(source); std::string line;
    while (std::getline(lines,line)) if (line.rfind("# @effect ",0)==0) {
      std::istringstream fields(line.substr(10)); std::vector<std::string> values; std::string value;
      while (std::getline(fields,value,'|')) values.push_back(value);
      if (values.size()==6) values.push_back("");
      if (values.size()!=7) throw std::runtime_error("Invalid company rule effect: " + line);
      RuleResult e; e.id=values[0]; e.cost=std::stoi(values[1]); e.days=std::stoi(values[2]);
      e.trust=std::stoi(values[3]); e.perCustomer=std::stoi(values[4]); e.breach=std::stoi(values[5])!=0; e.action=values[6];
      if (!effects.emplace(e.id,e).second) throw std::runtime_error("Duplicate company effect: " + e.id);
    }
    if (!narrator.loadFile(path.substr(0,path.rfind('.'))+".narration",diagnostics)) throw std::runtime_error("Cannot load company narration");
    const auto narrated=narrator.namedRules();
    for (const auto& id : program.ruleNames) {
      if (!effects.count(baseRule(id))) throw std::runtime_error("Missing effect for rule: " + id);
      if (!narrated.count(baseRule(id))) throw std::runtime_error("Missing narration for rule: " + id);
    }
  }
};
}
std::string Game::graphFacts() const {
  std::ostringstream out;
  const auto config = [&](const std::string& node,const std::string& value) { out << node << " configured " << value << '\n'; };
  const std::map<std::string,std::string> ids={{"E","Edge"},{"S","Store"},{"W","Worker"},{"P","Policy"},{"I","Identity"},{"R","Recovery"},{"O","Observe"}};
  const auto name=[&](const std::string& id) { const auto it=ids.find(id); return it==ids.end()?id:it->second; };
  for(const auto& node:architectureNodes()) out << "Company owns " << name(node.label) << '\n';
  for(const auto& edge:architectureEdges()) out << name(edge.first) << " calls " << name(edge.second) << '\n';
  config("Edge","delivery"+std::to_string(s.delivery));
  config("Store","storage"+std::to_string(s.storage)); config("Store","licensed");
  config("Worker","encoding"+std::to_string(s.encoding));
  config("Policy","scaling"+std::to_string(s.scaling)); config("Identity","identity"+std::to_string(s.scopedIdentity));
  config("Observe","monitoring"+std::to_string(s.monitoring)); config("Recovery","recovery"+std::to_string(s.recovery));
  config("Recovery","tested"+std::to_string(s.testedRecovery));
  config("Company",s.phase==Phase::Development?"development":s.phase==Phase::Launch?"launch":"maintenance");
  config("Company","week"+std::to_string(s.week));
  if(s.week==20) config("Company","alienweek");
  if(s.week>=7 && (s.week-7)%8==0) config("Company","renewal");
  if(s.week%4==0) config("Company","reviewweek");
  config("Company",s.business==Business::Video?"video":"game");
  if(s.week<4) config("Company","early");
  if(readyToLaunch()) config("Company","ready");
  if(s.week%8==0) config("Company","intel");
  config("Company","slot"+std::to_string(std::max(0,s.week-1)%16));
  if(availableTasks().size()>planners().size() && s.team.size()<6) config("Company","understaffed");
  if(s.week-s.developmentStart>s.developmentWeeks) config("Company","overdue");
  out << designFacts();
  return out.str();
}
RuleResult Game::rule(const std::string& domain,const std::string& facts) const {
  static const RuleBook book;
  graphrewrite::DiagnosticSink diagnostics;
  graphrewrite::runtime::LoadedFacts world;
  if(!graphrewrite::runtime::loadFactsText(graphFacts()+"Company evaluates "+domain+"\n"+facts,book.program.edgeTypes,"company snapshot",diagnostics,world)) {
    std::string errors;
    for(const auto& d:diagnostics.all()) errors+=d.format("company snapshot")+"\n";
    throw std::runtime_error(errors);
  }
  world.names.at(world.ids.at("Company"))=s.companyName;
  for(const auto& api:s.design.apis) if(world.ids.count(api.id)) world.names[world.ids.at(api.id)]=api.name+" ["+api.id+"]";
  for(const auto& data:s.design.data) if(world.ids.count(data.id)) world.names[world.ids.at(data.id)]=data.name+" ["+data.id+"]";
  graphrewrite::runtime::StorySettings settings;
  uint32_t querySeed=s.storySeed ^ (static_cast<uint32_t>(s.week)*2654435761u);
  for(unsigned char ch:domain) querySeed=(querySeed ^ ch)*16777619u;
  settings.seed=querySeed; settings.ticks=1; settings.populationCap=0; settings.rowCapacityPerTable=128;
  auto narrator=book.narrator; narrator.setGrammarSeed(querySeed);
  graphrewrite::runtime::StoryRunner runner(book.program,book.usage,world,narrator,settings);
  runner.step();
  if(runner.overflowedWeeks()) throw std::runtime_error("Company rule match overflow");
  uint32_t selected=~uint32_t(0);
  for(const auto& event:runner.events()) selected=std::min(selected,event.rule);
  if(selected==~uint32_t(0)) return {};
  auto result=book.effects.at(baseRule(book.program.ruleNames.at(selected)));
  {
    std::vector<graphrewrite::runtime::FiredEvent> chosen;
    for(const auto& event:runner.events()) if(event.rule==selected && (chosen.empty() || event.bound<chosen.front().bound)) chosen={event};
    const auto& variables=book.program.ruleVertexNames.at(selected);
    for(size_t i=0;i<variables.size() && i<chosen.front().bound.size();++i) {
      for(const auto& name:world.ids) if(name.second==chosen.front().bound[i]) result.bindings[variables[i]]=name.first;
    }
    for(const auto& tick:narrator.narrate(chosen,book.program,runner.names(),[&](uint32_t id){return runner.genderOf(id);}))
      for(const auto& sentence:tick.sentences) result.text+=(result.text.empty()?"":"\n")+sentence;
  }
  return result;
}
std::string Game::hiringAdvice() const { return rule("hiring").text; }
}
