/* Copyright (c) 2010, 2012-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/instr/instr_private.h"

#include "src/kernel/routing/AsImpl.hpp"
#include "simgrid/s4u/engine.hpp"
#include "surf/surf.h"
#include "src/surf/xml/platf_private.hpp"
#include "xbt/graph.h"

XBT_LOG_NEW_DEFAULT_SUBCATEGORY (instr_routing, instr, "Tracing platform hierarchy");

static int platform_created = 0;            /* indicate whether the platform file has been traced */
static std::vector<container_t> currentContainer; /* push and pop, used only in creation */

static const char *instr_node_name (xbt_node_t node)
{
  void *data = xbt_graph_node_get_data(node);
  char *str = (char*)data;
  return str;
}

static container_t lowestCommonAncestor (container_t a1, container_t a2)
{
  //this is only an optimization (since most of a1 and a2 share the same parent)
  if (a1->father == a2->father) return a1->father;

  //create an array with all ancestors of a1
  std::vector<container_t> ancestors_a1;
  container_t p;
  p = a1->father;
  while (p){
    ancestors_a1.push_back(p);
    p = p->father;
  }

  //create an array with all ancestors of a2
  std::vector<container_t> ancestors_a2;
  p = a2->father;
  while (p){
    ancestors_a2.push_back(p);
    p = p->father;
  }

  //find the lowest ancestor
  p = nullptr;
  int i = ancestors_a1.size() - 1;
  int j = ancestors_a2.size() - 1;
  while (i >= 0 && j >= 0){
    container_t a1p = ancestors_a1.at(i);
    container_t a2p = ancestors_a2.at(j);
    if (a1p == a2p){
      p = a1p;
    }else{
      break;
    }
    i--;
    j--;
  }
  return p;
}

static void linkContainers (container_t src, container_t dst, xbt_dict_t filter)
{
  //ignore loopback
  if (strcmp (src->name, "__loopback__") == 0 || strcmp (dst->name, "__loopback__") == 0){
    XBT_DEBUG ("  linkContainers: ignoring loopback link");
    return;
  }

  //find common father
  container_t father = lowestCommonAncestor (src, dst);
  if (!father){
    xbt_die ("common father unknown, this is a tracing problem");
  }

  if (filter != nullptr){
    //check if we already register this pair (we only need one direction)
    char aux1[INSTR_DEFAULT_STR_SIZE], aux2[INSTR_DEFAULT_STR_SIZE];
    snprintf (aux1, INSTR_DEFAULT_STR_SIZE, "%s%s", src->name, dst->name);
    snprintf (aux2, INSTR_DEFAULT_STR_SIZE, "%s%s", dst->name, src->name);
    if (xbt_dict_get_or_null (filter, aux1)){
      XBT_DEBUG ("  linkContainers: already registered %s <-> %s (1)", src->name, dst->name);
      return;
    }
    if (xbt_dict_get_or_null (filter, aux2)){
      XBT_DEBUG ("  linkContainers: already registered %s <-> %s (2)", dst->name, src->name);
      return;
    }

    //ok, not found, register it
    xbt_dict_set (filter, aux1, xbt_strdup ("1"), nullptr);
    xbt_dict_set (filter, aux2, xbt_strdup ("1"), nullptr);
  }

  //declare type
  char link_typename[INSTR_DEFAULT_STR_SIZE];
  snprintf (link_typename, INSTR_DEFAULT_STR_SIZE, "%s-%s%s-%s%s",
            father->type->name,
            src->type->name, src->type->id,
            dst->type->name, dst->type->id);
  type_t link_type = PJ_type_get_or_null (link_typename, father->type);
  if (link_type == nullptr){
    link_type = PJ_type_link_new (link_typename, father->type, src->type, dst->type);
  }

  //register EDGE types for triva configuration
  xbt_dict_set (trivaEdgeTypes, link_type->name, xbt_strdup("1"), nullptr);

  //create the link
  static long long counter = 0;

  char key[INSTR_DEFAULT_STR_SIZE];
  snprintf (key, INSTR_DEFAULT_STR_SIZE, "%lld", counter++);
  new_pajeStartLink(SIMIX_get_clock(), father, link_type, src, "topology", key);
  new_pajeEndLink(SIMIX_get_clock(), father, link_type, dst, "topology", key);

  XBT_DEBUG ("  linkContainers %s <-> %s", src->name, dst->name);
}

static void recursiveGraphExtraction (simgrid::s4u::As *as, container_t container, xbt_dict_t filter)
{
  if (!TRACE_platform_topology()){
    XBT_DEBUG("Graph extraction disabled by user.");
    return;
  }
  XBT_DEBUG ("Graph extraction for routing_component = %s", as->name());
  if (!xbt_dict_is_empty(as->children())){
    xbt_dict_cursor_t cursor = nullptr;
    AS_t rc_son;
    char *child_name;
    //bottom-up recursion
    xbt_dict_foreach(as->children(), cursor, child_name, rc_son) {
      container_t child_container = (container_t) xbt_dict_get (container->children, rc_son->name());
      recursiveGraphExtraction (rc_son, child_container, filter);
    }
  }

  {
    xbt_graph_t graph = xbt_graph_new_graph (0, nullptr);
    xbt_dict_t nodes = xbt_dict_new_homogeneous(nullptr);
    xbt_dict_t edges = xbt_dict_new_homogeneous(nullptr);
    xbt_edge_t edge = nullptr;

    xbt_dict_cursor_t cursor = nullptr;
    char *edge_name;

    static_cast<simgrid::kernel::routing::AsImpl*>(as)->getGraph(graph, nodes, edges);
    xbt_dict_foreach(edges,cursor,edge_name,edge) {
        linkContainers(
          PJ_container_get((const char*) edge->src->data),
          PJ_container_get((const char*) edge->dst->data), filter);
    }
    xbt_dict_free (&nodes);
    xbt_dict_free (&edges);
    xbt_graph_free_graph(graph, xbt_free_f, xbt_free_f, nullptr);
  }
}

/*
 * Callbacks
 */
void sg_instr_AS_begin(sg_platf_AS_cbarg_t AS)
{
  const char*id = AS->id;

  if (PJ_container_get_root() == nullptr){
    PJ_container_alloc ();
    PJ_type_alloc();
    container_t root = PJ_container_new (id, INSTR_AS, nullptr);
    PJ_container_set_root (root);

    if (TRACE_smpi_is_enabled()) {
      type_t mpi = PJ_type_get_or_null ("MPI", root->type);
      if (mpi == nullptr){
        mpi = PJ_type_container_new("MPI", root->type);
        if (!TRACE_smpi_is_grouped()) PJ_type_state_new ("MPI_STATE", mpi);
        PJ_type_link_new ("MPI_LINK", PJ_type_get_root(), mpi, mpi);
      }
    }

    if (TRACE_needs_platform()){
      currentContainer.push_back(root);
    }
    return;
  }

  if (TRACE_needs_platform()){
    container_t father = currentContainer.back();
    container_t container = PJ_container_new (id, INSTR_AS, father);
    currentContainer.push_back(container);
  }
}

void sg_instr_AS_end()
{
  if (TRACE_needs_platform()){
    currentContainer.pop_back();
  }
}

static void instr_routing_parse_start_link (sg_platf_link_cbarg_t link)
{
  container_t father = currentContainer.back();

  double bandwidth_value = link->bandwidth;
  double latency_value = link->latency;
  std::vector<std::string> links_to_create;

  if (link->policy == SURF_LINK_FULLDUPLEX){
    std::string id (link->id);
    std::string up = id + "_UP";
    std::string down = id + "_DOWN";
    links_to_create.push_back(up);
    links_to_create.push_back(down);
  }else{
    links_to_create.push_back(link->id);
  }

  for (auto link_name: links_to_create){
    container_t container = PJ_container_new (link_name.c_str(), INSTR_LINK, father);

    if ((TRACE_categorized() || TRACE_uncategorized() || TRACE_platform()) && (! TRACE_disable_link())) {
      type_t bandwidth = PJ_type_get_or_null ("bandwidth", container->type);
      if (bandwidth == nullptr){
        bandwidth = PJ_type_variable_new ("bandwidth", nullptr, container->type);
      }
      type_t latency = PJ_type_get_or_null ("latency", container->type);
      if (latency == nullptr){
        latency = PJ_type_variable_new ("latency", nullptr, container->type);
      }
      new_pajeSetVariable (0, container, bandwidth, bandwidth_value);
      new_pajeSetVariable (0, container, latency, latency_value);
    }
    if (TRACE_uncategorized()){
      type_t bandwidth_used = PJ_type_get_or_null ("bandwidth_used", container->type);
      if (bandwidth_used == nullptr){
        PJ_type_variable_new ("bandwidth_used", "0.5 0.5 0.5", container->type);
      }
    }
  }
}

void sg_instr_new_host(sg_platf_host_cbarg_t host)
{
  container_t father = currentContainer.back();
  container_t container = PJ_container_new (host->id, INSTR_HOST, father);

  if ((TRACE_categorized() || TRACE_uncategorized() || TRACE_platform()) && (! TRACE_disable_speed())) {
    type_t speed = PJ_type_get_or_null ("power", container->type);
    if (speed == nullptr){
      speed = PJ_type_variable_new ("power", nullptr, container->type);
    }

    double current_speed_state = host->speed_per_pstate[host->pstate];
    new_pajeSetVariable (0, container, speed, current_speed_state);
  }
  if (TRACE_uncategorized()){
    type_t speed_used = PJ_type_get_or_null ("power_used", container->type);
    if (speed_used == nullptr){
      PJ_type_variable_new ("power_used", "0.5 0.5 0.5", container->type);
    }
  }

  if (TRACE_smpi_is_enabled() && TRACE_smpi_is_grouped()){
    type_t mpi = PJ_type_get_or_null ("MPI", container->type);
    if (mpi == nullptr){
      mpi = PJ_type_container_new("MPI", container->type);
      PJ_type_state_new ("MPI_STATE", mpi);
    }
  }

  if (TRACE_msg_process_is_enabled()) {
    type_t msg_process = PJ_type_get_or_null ("MSG_PROCESS", container->type);
    if (msg_process == nullptr){
      msg_process = PJ_type_container_new("MSG_PROCESS", container->type);
      type_t state = PJ_type_state_new ("MSG_PROCESS_STATE", msg_process);
      PJ_value_new ("suspend", "1 0 1", state);
      PJ_value_new ("sleep", "1 1 0", state);
      PJ_value_new ("receive", "1 0 0", state);
      PJ_value_new ("send", "0 0 1", state);
      PJ_value_new ("task_execute", "0 1 1", state);
      PJ_type_link_new ("MSG_PROCESS_LINK", PJ_type_get_root(), msg_process, msg_process);
      PJ_type_link_new ("MSG_PROCESS_TASK_LINK", PJ_type_get_root(), msg_process, msg_process);
    }
  }

  if (TRACE_msg_vm_is_enabled()) {
    type_t msg_vm = PJ_type_get_or_null ("MSG_VM", container->type);
    if (msg_vm == nullptr){
      msg_vm = PJ_type_container_new("MSG_VM", container->type);
      type_t state = PJ_type_state_new ("MSG_VM_STATE", msg_vm);
      PJ_value_new ("suspend", "1 0 1", state);
      PJ_value_new ("sleep", "1 1 0", state);
      PJ_value_new ("receive", "1 0 0", state);
      PJ_value_new ("send", "0 0 1", state);
      PJ_value_new ("task_execute", "0 1 1", state);
      PJ_type_link_new ("MSG_VM_LINK", PJ_type_get_root(), msg_vm, msg_vm);
      PJ_type_link_new ("MSG_VM_PROCESS_LINK", PJ_type_get_root(), msg_vm, msg_vm);
    }
  }

}

void sg_instr_new_router(sg_platf_router_cbarg_t router)
{
  container_t father = currentContainer.back();
  PJ_container_new (router->id, INSTR_ROUTER, father);
}

static void instr_routing_parse_end_platform ()
{
  currentContainer.clear();
  xbt_dict_t filter = xbt_dict_new_homogeneous(xbt_free_f);
  XBT_DEBUG ("Starting graph extraction.");
  recursiveGraphExtraction (simgrid::s4u::Engine::instance()->rootAs(), PJ_container_get_root(), filter);
  XBT_DEBUG ("Graph extraction finished.");
  xbt_dict_free(&filter);
  platform_created = 1;
  TRACE_paje_dump_buffer(1);
}

void instr_routing_define_callbacks ()
{
  if (!TRACE_is_enabled()) return;
  //always need the call backs to ASes (we need only the root AS),
  //to create the rootContainer and the rootType properly
  if (!TRACE_needs_platform()) return;
  simgrid::surf::on_link.connect(instr_routing_parse_start_link);
  simgrid::surf::on_postparse.connect(instr_routing_parse_end_platform);
}

/*
 * user categories support
 */
static void recursiveNewVariableType (const char *new_typename, const char *color, type_t root)
{
  if (!strcmp (root->name, "HOST")){
    char tnstr[INSTR_DEFAULT_STR_SIZE];
    snprintf (tnstr, INSTR_DEFAULT_STR_SIZE, "p%s", new_typename);
    PJ_type_variable_new (tnstr, color, root);
  }
  if (!strcmp (root->name, "MSG_VM")){
    char tnstr[INSTR_DEFAULT_STR_SIZE];
    snprintf (tnstr, INSTR_DEFAULT_STR_SIZE, "p%s", new_typename);
    PJ_type_variable_new (tnstr, color, root);
  }
 if (!strcmp (root->name, "LINK")){
    char tnstr[INSTR_DEFAULT_STR_SIZE];
    snprintf (tnstr, INSTR_DEFAULT_STR_SIZE, "b%s", new_typename);
    PJ_type_variable_new (tnstr, color, root);
  }
  xbt_dict_cursor_t cursor = nullptr;
  type_t child_type;
  char *name;
  xbt_dict_foreach(root->children, cursor, name, child_type) {
    recursiveNewVariableType (new_typename, color, child_type);
  }
}

void instr_new_variable_type (const char *new_typename, const char *color)
{
  recursiveNewVariableType (new_typename, color, PJ_type_get_root());
}

static void recursiveNewUserVariableType (const char *father_type, const char *new_typename, const char *color, type_t root)
{
  if (!strcmp (root->name, father_type)){
    PJ_type_variable_new (new_typename, color, root);
  }
  xbt_dict_cursor_t cursor = nullptr;
  type_t child_type;
  char *name;
  xbt_dict_foreach(root->children, cursor, name, child_type) {
    recursiveNewUserVariableType (father_type, new_typename, color, child_type);
  }
}

void instr_new_user_variable_type  (const char *father_type, const char *new_typename, const char *color)
{
  recursiveNewUserVariableType (father_type, new_typename, color, PJ_type_get_root());
}

static void recursiveNewUserStateType (const char *father_type, const char *new_typename, type_t root)
{
  if (!strcmp (root->name, father_type)){
    PJ_type_state_new (new_typename, root);
  }
  xbt_dict_cursor_t cursor = nullptr;
  type_t child_type;
  char *name;
  xbt_dict_foreach(root->children, cursor, name, child_type) {
    recursiveNewUserStateType (father_type, new_typename, child_type);
  }
}

void instr_new_user_state_type (const char *father_type, const char *new_typename)
{
  recursiveNewUserStateType (father_type, new_typename, PJ_type_get_root());
}

static void recursiveNewValueForUserStateType (const char *type_name, const char *value, const char *color, type_t root)
{
  if (!strcmp (root->name, type_name)){
    PJ_value_new (value, color, root);
  }
  xbt_dict_cursor_t cursor = nullptr;
  type_t child_type;
  char *name;
  xbt_dict_foreach(root->children, cursor, name, child_type) {
    recursiveNewValueForUserStateType (type_name, value, color, child_type);
  }
}

void instr_new_value_for_user_state_type (const char *type_name, const char *value, const char *color)
{
  recursiveNewValueForUserStateType (type_name, value, color, PJ_type_get_root());
}

int instr_platform_traced ()
{
  return platform_created;
}

#define GRAPHICATOR_SUPPORT_FUNCTIONS

static void recursiveXBTGraphExtraction (xbt_graph_t graph, xbt_dict_t nodes, xbt_dict_t edges,
    AS_t as, container_t container)
{
  if (!xbt_dict_is_empty(as->children())){
    xbt_dict_cursor_t cursor = nullptr;
    AS_t as_child;
    char *child_name;
    //bottom-up recursion
    xbt_dict_foreach(as->children(), cursor, child_name, as_child) {
      container_t child_container = (container_t) xbt_dict_get (
        container->children, as_child->name());
      recursiveXBTGraphExtraction (graph, nodes, edges, as_child, child_container);
    }
  }

  static_cast<simgrid::kernel::routing::AsImpl*>(as)->getGraph(graph, nodes, edges);
}

xbt_graph_t instr_routing_platform_graph ()
{
  xbt_graph_t ret = xbt_graph_new_graph (0, nullptr);
  xbt_dict_t nodes = xbt_dict_new_homogeneous(nullptr);
  xbt_dict_t edges = xbt_dict_new_homogeneous(nullptr);
  recursiveXBTGraphExtraction (ret, nodes, edges, simgrid::s4u::Engine::instance()->rootAs(), PJ_container_get_root());
  xbt_dict_free (&nodes);
  xbt_dict_free (&edges);
  return ret;
}

void instr_routing_platform_graph_export_graphviz (xbt_graph_t g, const char *filename)
{
  unsigned int cursor = 0;
  xbt_node_t node = nullptr;
  xbt_edge_t edge = nullptr;
  FILE *file = nullptr;

  file = fopen(filename, "w");
  xbt_assert(file, "Failed to open %s \n", filename);

  if (g->directed)
    fprintf(file, "digraph test {\n");
  else
    fprintf(file, "graph test {\n");

  fprintf(file, "  graph [overlap=scale]\n");

  fprintf(file, "  node [shape=box, style=filled]\n");
  fprintf(file,
          "  node [width=.3, height=.3, style=filled, color=skyblue]\n\n");

  xbt_dynar_foreach(g->nodes, cursor, node) {
    fprintf(file, "  \"%s\";\n", instr_node_name(node));
  }
  xbt_dynar_foreach(g->edges, cursor, edge) {
    const char *src_s = instr_node_name (edge->src);
    const char *dst_s = instr_node_name (edge->dst);
    if (g->directed)
      fprintf(file, "  \"%s\" -> \"%s\";\n", src_s, dst_s);
    else
      fprintf(file, "  \"%s\" -- \"%s\";\n", src_s, dst_s);
  }
  fprintf(file, "}\n");
  fclose(file);

}
