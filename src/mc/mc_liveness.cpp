/* Copyright (c) 2011-2015. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <cstring>

#include <unistd.h>
#include <sys/wait.h>

#include <xbt/automaton.h>
#include <xbt/dynar.h>
#include <xbt/fifo.h>
#include <xbt/log.h>
#include <xbt/parmap.h>
#include <xbt/sysdep.h>

#include "src/mc/mc_request.h"
#include "src/mc/mc_liveness.h"
#include "src/mc/mc_private.h"
#include "src/mc/mc_record.h"
#include "src/mc/mc_smx.h"
#include "src/mc/mc_client.h"
#include "src/mc/mc_replay.h"
#include "src/mc/mc_safety.h"
#include "src/mc/mc_exit.h"

extern "C" {

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_liveness, mc,
                                "Logging specific to algorithms for liveness properties verification");

/********* Global variables *********/

xbt_dynar_t acceptance_pairs;
xbt_parmap_t parmap;

/********* Static functions *********/

static xbt_dynar_t get_atomic_propositions_values()
{
  unsigned int cursor = 0;
  xbt_automaton_propositional_symbol_t ps = nullptr;
  xbt_dynar_t values = xbt_dynar_new(sizeof(int), nullptr);
  xbt_dynar_foreach(_mc_property_automaton->propositional_symbols, cursor, ps) {
    int res = xbt_automaton_propositional_symbol_evaluate(ps);
    xbt_dynar_push_as(values, int, res);
  }

  return values;
}

static mc_visited_pair_t is_reached_acceptance_pair(mc_pair_t pair)
{
  mc_visited_pair_t new_pair = nullptr;
  new_pair = MC_visited_pair_new(pair->num, pair->automaton_state, pair->atomic_propositions, pair->graph_state);
  new_pair->acceptance_pair = 1;

  if (xbt_dynar_is_empty(acceptance_pairs)) {

    xbt_dynar_push(acceptance_pairs, &new_pair);

  } else {

    int min = -1, max = -1, index;
    //int res;
    mc_visited_pair_t pair_test;
    int cursor;

    index = get_search_interval(acceptance_pairs, new_pair, &min, &max);

    if (min != -1 && max != -1) {       // Acceptance pair with same number of processes and same heap bytes used exists

      // Parallell implementation
      /*res = xbt_parmap_mc_apply(parmap, snapshot_compare, xbt_dynar_get_ptr(acceptance_pairs, min), (max-min)+1, pair);
         if(res != -1){
         return ((mc_pair_t)xbt_dynar_get_as(acceptance_pairs, (min+res)-1, mc_pair_t))->num;
         } */

      cursor = min;
      if(pair->search_cycle == 1){
        while (cursor <= max) {
          pair_test = (mc_visited_pair_t) xbt_dynar_get_as(acceptance_pairs, cursor, mc_visited_pair_t);
          if (xbt_automaton_state_compare(pair_test->automaton_state, new_pair->automaton_state) == 0) {
            if (xbt_automaton_propositional_symbols_compare_value(pair_test->atomic_propositions, new_pair->atomic_propositions) == 0) {
              if (snapshot_compare(pair_test, new_pair) == 0) {
                XBT_INFO("Pair %d already reached (equal to pair %d) !", new_pair->num, pair_test->num);
                xbt_fifo_shift(mc_stack);
                if (dot_output != nullptr)
                  fprintf(dot_output, "\"%d\" -> \"%d\" [%s];\n", initial_global_state->prev_pair, pair_test->num, initial_global_state->prev_req);
                return nullptr;
              }
            }
          }
          cursor++;
        }
      }
      xbt_dynar_insert_at(acceptance_pairs, min, &new_pair);
    } else {
      pair_test = (mc_visited_pair_t) xbt_dynar_get_as(acceptance_pairs, index, mc_visited_pair_t);
      if (pair_test->nb_processes < new_pair->nb_processes) {
        xbt_dynar_insert_at(acceptance_pairs, index + 1, &new_pair);
      } else {
        if (pair_test->heap_bytes_used < new_pair->heap_bytes_used)
          xbt_dynar_insert_at(acceptance_pairs, index + 1, &new_pair);
        else
          xbt_dynar_insert_at(acceptance_pairs, index, &new_pair);
      }
    }
  }
  return new_pair;
}

static void remove_acceptance_pair(int pair_num)
{
  unsigned int cursor = 0;
  mc_visited_pair_t pair_test = nullptr;
  int pair_found = 0;

  xbt_dynar_foreach(acceptance_pairs, cursor, pair_test) {
    if (pair_test->num == pair_num) {
       pair_found = 1;
      break;
    }
  }

  if(pair_found == 1) {
    xbt_dynar_remove_at(acceptance_pairs, cursor, &pair_test);

    pair_test->acceptance_removed = 1;

    if (_sg_mc_visited == 0 || pair_test->visited_removed == 1) 
      MC_visited_pair_delete(pair_test);

  }
}


static int MC_automaton_evaluate_label(xbt_automaton_exp_label_t l,
                                       xbt_dynar_t atomic_propositions_values)
{

  switch (l->type) {
  case 0:{
      int left_res = MC_automaton_evaluate_label(l->u.or_and.left_exp, atomic_propositions_values);
      int right_res = MC_automaton_evaluate_label(l->u.or_and.right_exp, atomic_propositions_values);
      return (left_res || right_res);
    }
  case 1:{
      int left_res = MC_automaton_evaluate_label(l->u.or_and.left_exp, atomic_propositions_values);
      int right_res = MC_automaton_evaluate_label(l->u.or_and.right_exp, atomic_propositions_values);
      return (left_res && right_res);
    }
  case 2:{
      int res = MC_automaton_evaluate_label(l->u.exp_not, atomic_propositions_values);
      return (!res);
    }
  case 3:{
      unsigned int cursor = 0;
      xbt_automaton_propositional_symbol_t p = nullptr;
      xbt_dynar_foreach(_mc_property_automaton->propositional_symbols, cursor, p) {
        if (std::strcmp(xbt_automaton_propositional_symbol_get_name(p), l->u.predicat) == 0)
          return (int) xbt_dynar_get_as(atomic_propositions_values, cursor, int);
      }
      return -1;
    }
  case 4:{
      return 2;
    }
  default:
    return -1;
  }
}

static int MC_modelcheck_liveness_main(void);

static void MC_pre_modelcheck_liveness(void)
{
  mc_pair_t initial_pair = nullptr;
  mc_model_checker->wait_for_requests();
  acceptance_pairs = xbt_dynar_new(sizeof(mc_visited_pair_t), nullptr);
  if(_sg_mc_visited > 0)
    visited_pairs = xbt_dynar_new(sizeof(mc_visited_pair_t), nullptr);

  initial_global_state->snapshot = simgrid::mc::take_snapshot(0);
  initial_global_state->prev_pair = 0;

  unsigned int cursor = 0;
  xbt_automaton_state_t automaton_state;
  
  xbt_dynar_foreach(_mc_property_automaton->states, cursor, automaton_state) {
    if (automaton_state->type == -1) {  /* Initial automaton state */

      initial_pair = MC_pair_new();
      initial_pair->automaton_state = automaton_state;
      initial_pair->graph_state = MC_state_new();
      initial_pair->atomic_propositions = get_atomic_propositions_values();
      initial_pair->depth = 1;

      /* Get enabled processes and insert them in the interleave set of the graph_state */
      for (auto& p : mc_model_checker->process().simix_processes())
        if (MC_process_is_enabled(&p.copy))
          MC_state_interleave_process(initial_pair->graph_state, &p.copy);

      initial_pair->requests = MC_state_interleave_size(initial_pair->graph_state);
      initial_pair->search_cycle = 0;

      xbt_fifo_unshift(mc_stack, initial_pair);
    }
  }
}

static int MC_modelcheck_liveness_main(void)
{
  mc_pair_t current_pair = nullptr;
  int value, res, visited_num = -1;
  smx_simcall_t req = nullptr;
  xbt_automaton_transition_t transition_succ = nullptr;
  int cursor = 0;
  mc_pair_t next_pair = nullptr;
  xbt_dynar_t prop_values = nullptr;
  mc_visited_pair_t reached_pair = nullptr;
  
  while(xbt_fifo_size(mc_stack) > 0){

    /* Get current pair */
    current_pair = (mc_pair_t) xbt_fifo_get_item_content(xbt_fifo_get_first_item(mc_stack));

    /* Update current state in buchi automaton */
    _mc_property_automaton->current_state = current_pair->automaton_state;

    XBT_DEBUG("********************* ( Depth = %d, search_cycle = %d, interleave size = %d, pair_num = %d, requests = %d)",
       current_pair->depth, current_pair->search_cycle,
       MC_state_interleave_size(current_pair->graph_state), current_pair->num,
       current_pair->requests);

    if (current_pair->requests > 0) {

      if (current_pair->automaton_state->type == 1 && current_pair->exploration_started == 0) {
        /* If new acceptance pair, return new pair */
        if ((reached_pair = is_reached_acceptance_pair(current_pair)) == nullptr) {
          int counter_example_depth = current_pair->depth;
          XBT_INFO("*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*");
          XBT_INFO("|             ACCEPTANCE CYCLE            |");
          XBT_INFO("*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*");
          XBT_INFO("Counter-example that violates formula :");
          MC_record_dump_path(mc_stack);
          MC_show_stack_liveness(mc_stack);
          MC_dump_stack_liveness(mc_stack);
          MC_print_statistics(mc_stats);
          XBT_INFO("Counter-example depth : %d", counter_example_depth);
          return SIMGRID_MC_EXIT_LIVENESS;
        }
      }

      /* Pair already visited ? stop the exploration on the current path */
      if ((current_pair->exploration_started == 0) && (visited_num = is_visited_pair(reached_pair, current_pair)) != -1) {

        if (dot_output != nullptr){
          fprintf(dot_output, "\"%d\" -> \"%d\" [%s];\n", initial_global_state->prev_pair, visited_num, initial_global_state->prev_req);
          fflush(dot_output);
        }

        XBT_DEBUG("Pair already visited (equal to pair %d), exploration on the current path stopped.", visited_num);
        current_pair->requests = 0;
        goto backtracking;
        
      }else{

        req = MC_state_get_request(current_pair->graph_state, &value);

         if (dot_output != nullptr) {
           if (initial_global_state->prev_pair != 0 && initial_global_state->prev_pair != current_pair->num) {
             fprintf(dot_output, "\"%d\" -> \"%d\" [%s];\n", initial_global_state->prev_pair, current_pair->num, initial_global_state->prev_req);
             xbt_free(initial_global_state->prev_req);
           }
           initial_global_state->prev_pair = current_pair->num;
           initial_global_state->prev_req = MC_request_get_dot_output(req, value);
           if (current_pair->search_cycle)
             fprintf(dot_output, "%d [shape=doublecircle];\n", current_pair->num);
           fflush(dot_output);
         }

         char* req_str = MC_request_to_string(req, value, MC_REQUEST_SIMIX); 
         XBT_DEBUG("Execute: %s", req_str);
         xbt_free(req_str);

         /* Set request as executed */
         MC_state_set_executed_request(current_pair->graph_state, req, value);

         /* Update mc_stats */
         mc_stats->executed_transitions++;
         if(current_pair->exploration_started == 0)
           mc_stats->visited_pairs++;

         /* Answer the request */
         MC_simcall_handle(req, value);
         
         /* Wait for requests (schedules processes) */
         mc_model_checker->wait_for_requests();

         current_pair->requests--;
         current_pair->exploration_started = 1;

         /* Get values of atomic propositions (variables used in the property formula) */ 
         prop_values = get_atomic_propositions_values();

         /* Evaluate enabled/true transitions in automaton according to atomic propositions values and create new pairs */
         cursor = xbt_dynar_length(current_pair->automaton_state->out) - 1;
         while (cursor >= 0) {
           transition_succ = (xbt_automaton_transition_t)xbt_dynar_get_as(current_pair->automaton_state->out, cursor, xbt_automaton_transition_t);
           res = MC_automaton_evaluate_label(transition_succ->label, prop_values);
           if (res == 1 || res == 2) { /* 1 = True transition (always enabled), 2 = enabled transition according to atomic prop values */
              next_pair = MC_pair_new();
              next_pair->graph_state = MC_state_new();
              next_pair->automaton_state = transition_succ->dst;
              next_pair->atomic_propositions = get_atomic_propositions_values();
              next_pair->depth = current_pair->depth + 1;
              /* Get enabled processes and insert them in the interleave set of the next graph_state */
              for (auto& p : mc_model_checker->process().simix_processes())
                if (MC_process_is_enabled(&p.copy))
                  MC_state_interleave_process(next_pair->graph_state, &p.copy);

              next_pair->requests = MC_state_interleave_size(next_pair->graph_state);

              /* FIXME : get search_cycle value for each acceptant state */
              if (next_pair->automaton_state->type == 1 || current_pair->search_cycle)
                next_pair->search_cycle = 1;

              /* Add new pair to the exploration stack */
              xbt_fifo_unshift(mc_stack, next_pair);

           }
           cursor--;
         }

      } /* End of visited_pair test */
      
    } else {

    backtracking:
      if(visited_num == -1)
        XBT_DEBUG("No more request to execute. Looking for backtracking point.");
    
      xbt_dynar_free(&prop_values);
    
      /* Traverse the stack backwards until a pair with a non empty interleave
         set is found, deleting all the pairs that have it empty in the way. */
      while ((current_pair = (mc_pair_t) xbt_fifo_shift(mc_stack)) != nullptr) {
        if (current_pair->requests > 0) {
          /* We found a backtracking point */
          XBT_DEBUG("Backtracking to depth %d", current_pair->depth);
          xbt_fifo_unshift(mc_stack, current_pair);
          MC_replay_liveness(mc_stack);
          XBT_DEBUG("Backtracking done");
          break;
        }else{
          /* Delete pair */
          XBT_DEBUG("Delete pair %d at depth %d", current_pair->num, current_pair->depth);
          if (current_pair->automaton_state->type == 1) 
            remove_acceptance_pair(current_pair->num);
          MC_pair_delete(current_pair);
        }
      }

    } /* End of if (current_pair->requests > 0) else ... */
    
  } /* End of while(xbt_fifo_size(mc_stack) > 0) */

  XBT_INFO("No property violation found.");
  MC_print_statistics(mc_stats);
  return SIMGRID_MC_EXIT_SUCCESS;
}

int MC_modelcheck_liveness(void)
{
  if (mc_reduce_kind == e_mc_reduce_unset)
    mc_reduce_kind = e_mc_reduce_none;
  XBT_INFO("Check the liveness property %s", _sg_mc_property_file);
  MC_automaton_load(_sg_mc_property_file);
  mc_model_checker->wait_for_requests();

  XBT_DEBUG("Starting the liveness algorithm");
  _sg_mc_liveness = 1;

  /* Create exploration stack */
  mc_stack = xbt_fifo_new();

  /* Create the initial state */
  initial_global_state = xbt_new0(s_mc_global_t, 1);

  MC_pre_modelcheck_liveness();
  int res = MC_modelcheck_liveness_main();

  /* We're done */
  xbt_free(mc_time);

  return res;
}

}
