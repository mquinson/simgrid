/* Copyright (c) 2016. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "smpi/smpi_utils.hpp"
#include "xbt/sysdep.h"
#include "xbt/log.h"
#include "xbt/str.h"
#include <boost/tokenizer.hpp>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(smpi_utils, smpi, "Logging specific to SMPI (utils)");

std::vector<s_smpi_factor_t> parse_factor(const char *smpi_coef_string)
{
  std::vector<s_smpi_factor_t> smpi_factor;

  /** Setup the tokenizer that parses the string **/
  typedef boost::tokenizer<boost::char_separator<char>> Tokenizer;
  boost::char_separator<char> sep(";");
  boost::char_separator<char> factor_separator(":");
  std::string tmp_string(smpi_coef_string);
  Tokenizer tokens(tmp_string, sep);

  /**
   * Iterate over patterns like A:B:C:D;E:F;G:H
   * These will be broken down into:
   * A --> B, C, D
   * E --> F
   * G --> H
   */
  for (Tokenizer::iterator token_iter = tokens.begin(); token_iter != tokens.end(); token_iter++) {
    XBT_DEBUG("token : %s", token_iter->c_str());
    Tokenizer factor_values(*token_iter, factor_separator);
    s_smpi_factor_t fact;
    if (factor_values.begin() == factor_values.end()) {
      xbt_die("Malformed radical for smpi factor: '%s'", smpi_coef_string);
    }
    unsigned int iteration = 0;
    for (Tokenizer::iterator factor_iter = factor_values.begin(); factor_iter != factor_values.end(); factor_iter++) {
      iteration++;
      char *errmsg;

      if (factor_iter == factor_values.begin()) { /* first element */
        errmsg = bprintf("Invalid factor in chunk #%zu: %%s", smpi_factor.size()+1);
        fact.factor = xbt_str_parse_int(factor_iter->c_str(), errmsg);
      } else {
        errmsg = bprintf("Invalid factor value %d in chunk #%zu: %%s", iteration, smpi_factor.size()+1);
        fact.values.push_back(xbt_str_parse_double(factor_iter->c_str(), errmsg));
      }
      xbt_free(errmsg);
    }

    smpi_factor.push_back(fact);
    XBT_DEBUG("smpi_factor:\t%zu : %zu values, first: %f", fact.factor, smpi_factor.size(), fact.values[0]);
  }
  std::sort(smpi_factor.begin(), smpi_factor.end(), [](const s_smpi_factor_t &pa, const s_smpi_factor_t &pb) {
    return (pa.factor < pb.factor);
  });
  for (auto& fact : smpi_factor) {
    XBT_DEBUG("smpi_factor:\t%zu : %zu values, first: %f", fact.factor, smpi_factor.size() ,fact.values[0]);
  }
  smpi_factor.shrink_to_fit();

  return smpi_factor;
}
