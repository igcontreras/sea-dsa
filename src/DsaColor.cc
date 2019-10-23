#include "sea_dsa/DsaColor.hh"

using namespace sea_dsa;
using namespace llvm;

// Color class

Color::Color(): m_r(0), m_g(0), m_b(0) {
  randColor();
}

Color::Color(int r, int g, int b) : m_r(r), m_g(g), m_b(b) {};

void Color::randColor() { // it would be nice to not generate the same colors all the time
  m_r = std::rand() % 255;
  m_g = std::rand() % 255;
  m_b = std::rand() % 255;
}

std::string Color::stringColor() const {
  std::ostringstream stringStream;

  stringStream << "\"#" << std::hex << m_r << m_g << m_b << "\"";
  return stringStream.str();
}


// What I actually want is a constructor that copies a graph but adds the two fields!
// Actually the copy is not necessary, I just want a pointer to the graph.
// ColorGraph class
ColoredGraph::ColoredGraph(Graph &g, ColorMap &colorM, SafeNodeSet &safe)
    : m_g(g), m_color(colorM), m_safe(safe) {};

sea_dsa::Graph & ColoredGraph::getGraph() { return m_g;}
sea_dsa::Graph &ColoredGraph::getGraph() const { return m_g; }

std::string ColoredGraph::getColorNode(const Node *n) const {

  std::string color_string;
  raw_string_ostream OS(color_string);

  auto it = m_color.find(n);

  // if it is not colored, it means that it was not mapped --> return gray
  if(it != m_color.end()){
    return it->getSecond().stringColor();
  }
  else{
    OS << "grey";
    return OS.str();
  }
}

bool ColoredGraph::isSafeNode(const Node *n) const {
  return m_safe.count(n) == 0;
}

// Coloring functions

std::unique_ptr<Graph> cloneGraph(const llvm::DataLayout &dl,
                                  Graph::SetFactory &sf,const Graph &g) {
  std::unique_ptr<Graph> new_g( new Graph( dl, sf, g.isFlat()));
  new_g->import(g, true /*copy all parameters*/);
  return std::move(new_g);
}

bool GraphExplorer::isSafeNode(SafeNodeSet &f_safe, const Node *n) {
  return f_safe.count(n) == 0;
}

void GraphExplorer::mark_nodes_graph(Graph &g, const Function &F,
                                     SafeNodeSet &f_safe, SafeNodeSet &f_safe_caller,
                                     SimulationMapper &sm) {
  ExplorationMap f_visited;

  for (const Argument &a : F.args()) {
    if (g.hasCell(a)) { // scalar arguments don't have cells
      const Cell &c = g.getCell(a);
      const Node *n = c.getNode();
      mark_copy(*n, f_visited, f_safe, f_safe_caller, sm);
    }
  }
}

bool GraphExplorer::mark_copy(const Node &n, ExplorationMap &f_color,
                              SafeNodeSet &f_safe, SafeNodeSet &f_safe_caller,
                              SimulationMapper &sm ) {
  f_color[&n] = GRAY;

  for (auto &links : n.getLinks()) {
    const Field &f = links.first;
    const Cell &next_c = *links.second;
    const Node *next_n = next_c.getNode();
    auto it = f_color.find(next_n);
    if (it == f_color.end() && mark_copy(*next_n, f_color, f_safe,f_safe_caller,sm)) {
      return true;
    } else if (it != f_color.end() && it->getSecond() == GRAY) {
      propagate_not_copy(n, f_color, f_safe,f_safe_caller,sm);
      return true;
    }
  }

  f_color[&n] = BLACK;

  return false;
}

void GraphExplorer::propagate_not_copy(const Node &n, ExplorationMap &f_color,
                                       SafeNodeSet &f_safe,
                                       SafeNodeSet &f_safe_caller,
                                       SimulationMapper &sm) {
  if(isSafeNode(f_safe,&n))
    f_safe.insert(&n); // we store the ones that are not safe

  f_color[&n] = BLACK; // TODO: change by insert?

  for (auto &links : n.getLinks()){
    const Field &f = links.first;
    const Cell &next_c = *links.second;
    const Node * next_n = next_c.getNode();

    auto next = f_color.find(next_n);

    bool explored = next != f_color.end() && next->getSecond() == BLACK;
    bool marked_safe = isSafeNode(f_safe, next_n);

    if (!(explored && !marked_safe)) {
      const Node &next_n_caller = *sm.get(next_c).getNode();

      if(isSafeNode(f_safe_caller,&next_n_caller))
         f_safe_caller.insert(&next_n_caller);

      propagate_not_copy(*next_n, f_color, f_safe, f_safe_caller, sm);
    }
  }
}

void GraphExplorer::color_nodes_graph(Graph &g, const Function &F,
                                      SimulationMapper &sm, ColorMap &c_callee,
                                      ColorMap &c_caller,
                                      SafeNodeSet f_node_safe_callee,
                                      SafeNodeSet f_node_safe_caller) {

  SafeNodeSet f_proc; // keep track of processed nodes
  for (const Argument &a : F.args()) {
    if (g.hasCell(a)) { // scalar arguments don't have cells
      const Cell &c = g.getCell(a);
      const Node *n = c.getNode();
      color_nodes_aux(*n, f_proc, sm, c_callee, c_caller,f_node_safe_callee,f_node_safe_caller);
    }
  }
}

void GraphExplorer::color_nodes_aux(const Node &n, SafeNodeSet &f_proc,
                                    SimulationMapper &sm, ColorMap &c_callee,
                                    ColorMap &c_caller,
                                    SafeNodeSet f_node_safe_callee,
                                    SafeNodeSet f_node_safe_caller) {

  f_proc.insert(&n); // mark processed

  for (auto &links : n.getLinks()) {
    const Field &f = links.first;
    const Cell &next_c_callee = *links.second;
    const Node * next_n_callee = next_c_callee.getNode();
    const Cell &next_c_caller = sm.get(next_c_callee);
    const Node *next_n_caller = next_c_caller.getNode();

    // update if the node is safe (w.r.t. the simulated node)
    if (!isSafeNode(f_node_safe_caller, next_n_caller)) {
      // add the node to the unsafe set
      if (isSafeNode(f_node_safe_caller, next_n_callee))
        f_node_safe_callee.insert(next_n_caller);
    }

    Color col;
    auto it = c_caller.find(next_n_caller);
    if (it != c_caller.end()) {
      col = it->second;
    } else {
      c_caller.insert(std::make_pair(next_n_caller, col));
    }
    c_callee.insert(std::make_pair(next_n_callee,col));

    if (f_proc.count(next_n_callee) == 0) { // not processed yet
      color_nodes_aux(*next_n_callee, f_proc, sm, c_callee, c_caller,
                      f_node_safe_callee, f_node_safe_caller);
    }
  }
}

void GraphExplorer::colorGraph(const DsaCallSite &cs, const Graph &calleeG,
                             const Graph &callerG, ColorMap &color_callee,
                               ColorMap &color_caller, SafeNodeSet &f_node_safe_callee) {

  SimulationMapper simMap;

  bool res = Graph::computeCalleeCallerMapping(cs, *(const_cast<Graph*>(&calleeG)), *(const_cast<Graph*>(&callerG)), simMap);
  SafeNodeSet f_node_safe_caller;

  mark_nodes_graph(*(const_cast<Graph *>(&calleeG)), *cs.getCallee(),
                   f_node_safe_callee, f_node_safe_caller, simMap);

  color_nodes_graph(*(const_cast<Graph *>(&calleeG)),*cs.getCallee(),simMap,color_callee,color_caller,f_node_safe_callee,f_node_safe_caller);
}

/************************************************************/
/* only safe exploration, no coloring!                      */
/************************************************************/

void GraphExplorer::getSafeNodesCallerGraph(const CallSite &cs,
                                            const Graph &calleeG,
                                            const Graph &callerG,
                                            SimulationMapper &simMap,
                                            SafeNodeSet &f_node_safe_caller) {

  DsaCallSite dsa_cs(*cs.getInstruction());

  bool res = Graph::computeCalleeCallerMapping(
      dsa_cs, *(const_cast<Graph *>(&calleeG)),
      *(const_cast<Graph *>(&callerG)), simMap);
  SafeNodeSet f_node_safe_callee;

  mark_nodes_graph(*(const_cast<Graph *>(&calleeG)), *dsa_cs.getCallee(),
                   f_node_safe_callee, f_node_safe_caller, simMap);
}
