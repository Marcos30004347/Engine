#include "Company.hpp"
#include <algorithm>
#include <sstream>
#include <vector>

namespace company {
namespace {
// Box layout with one orthogonal routing lane per edge. Shared AWS services
// remain one node; crossings are marked + and arrowheads enter destinations.
struct Node { std::string id,title; int layer; };
std::string render(const std::vector<Node>& nodes,const std::vector<std::pair<std::string,std::string>>& edges) {
  constexpr size_t width=32;
  std::vector<std::string> canvas;
  std::vector<int> rows;
  const size_t columns=width+5+edges.size()*2;
  for(const auto& node:nodes) {
    std::vector<std::string> text;
    std::istringstream words("["+node.id+"] "+node.title); std::string word,line;
    while(words>>word) {
      if(!line.empty() && line.size()+word.size()+1>width) { text.push_back(line); line.clear(); }
      while(word.size()>width) { text.push_back(word.substr(0,width)); word.erase(0,width); }
      if(!line.empty()) line+=' '; line+=word;
    }
    if(!line.empty()) text.push_back(line);
    rows.push_back(static_cast<int>(canvas.size())+1);
    canvas.push_back("+"+std::string(width+2,'-')+"+");
    for(const auto& row:text) canvas.push_back("| "+row+std::string(width-row.size(),' ')+" |");
    canvas.push_back("+"+std::string(width+2,'-')+"+");
    canvas.push_back("");
  }
  for(auto& row:canvas) row.resize(columns,' ');
  auto plot=[&](int x,int y,char ch) {
    auto& cell=canvas.at(y).at(x);
    cell=(cell!=' ' && cell!=ch)?'+':ch;
  };
  for(size_t i=0;i<edges.size();++i) {
    int from=-1,to=-1;
    for(size_t j=0;j<nodes.size();++j) { if(nodes[j].id==edges[i].first) from=rows[j]; if(nodes[j].id==edges[i].second) to=rows[j]; }
    if(from<0 || to<0) throw std::logic_error("Graph edge has no endpoint");
    const int start=width+4,lane=start+static_cast<int>(i)*2;
    for(int x=start;x<=lane;++x) { plot(x,from,'-'); plot(x,to,'-'); }
    for(int y=std::min(from,to);y<=std::max(from,to);++y) plot(lane,y,'|');
    canvas[to][start]='<';
  }
  std::ostringstream out;
  for(auto& row:canvas) { const auto end=row.find_last_not_of(' '); out << (end==std::string::npos?"":row.substr(0,end+1)) << '\n'; }
  out << "Connections: ";
  size_t used=13;
  for(const auto& edge:edges) {
    const auto link="["+edge.first+"] -> ["+edge.second+"]  ";
    if(used+link.size()>72) { out << '\n'; used=0; }
    out << link; used+=link.size();
  }
  out << '\n';
  return out.str();
}
}
std::string Game::systemGraph(const std::string& system) const {
  if(system.empty()) return "SYSTEM MAP / API ownership and backend connections\n"+designGraph();
  const auto labels=systemComponents(system); std::vector<Node> nodes;
  std::ostringstream settings;
  for(const auto& n:architectureNodes()) if(std::find(labels.begin(),labels.end(),n.label)!=labels.end()) {
    nodes.push_back({n.label,n.service,static_cast<int>(nodes.size())});
    settings << '[' << n.label << "] " << n.settings << " | $" << n.monthlyCost << "/month\n";
  }
  std::vector<std::pair<std::string,std::string>> edges;
  for(const auto& e:architectureEdges())
    if(std::find(labels.begin(),labels.end(),e.first)!=labels.end() && std::find(labels.begin(),labels.end(),e.second)!=labels.end()) edges.push_back(e);
  return "AWS DETAIL / system "+system+" / shared components\n"+render(nodes,edges)+settings.str()+"\nAPI ownership and data placement determine these connections.\n";
}
std::string Game::designGraph(const std::string& apiId) const {
  std::vector<Node> nodes;
  auto edges=designEdges();
  std::vector<std::string> selected;
  if(!apiId.empty()) {
    for(const auto& api:s.design.apis) if(api.id==apiId) {
      selected={"G",api.compute};
      for(const auto& id:api.data) for(const auto& data:s.design.data) if(data.id==id) selected.push_back(data.database);
    }
    if(selected.empty()) throw std::invalid_argument("Unknown API");
    edges.erase(std::remove_if(edges.begin(),edges.end(),[&](const auto& e){return
      std::find(selected.begin(),selected.end(),e.first)==selected.end() || std::find(selected.begin(),selected.end(),e.second)==selected.end();}),edges.end());
  }
  for(const auto& n:designNodes()) if(apiId.empty() || std::find(selected.begin(),selected.end(),n.label)!=selected.end()) nodes.push_back({n.label,n.service,0});
  std::ostringstream out;
  out << "SYSTEM DESIGN / " << (s.design.planned?"ownership recorded":"choose boundaries before AWS services")
      << " / projected cloud $" << cloudCost() << "/month\n" << render(nodes,edges);
  for(const auto& api:s.design.apis) if(apiId.empty() || api.id==apiId) {
    out << '[' << api.id << "] " << api.name << " -> [" << api.compute << "] | data ";
    for(const auto& id:api.data) for(const auto& data:s.design.data) if(data.id==id) out << '[' << id << " @ " << data.database << "] ";
    out << "\n    " << api.usage << '\n';
  }
  for(const auto& data:s.design.data) out << '[' << data.id << "] " << data.name << " -> [" << data.database << "] | " << data.access << " access\n";
  return out.str();
}

}
