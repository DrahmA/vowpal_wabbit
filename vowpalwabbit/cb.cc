/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */
#include <float.h>

#include "example.h"
#include "parse_primitives.h"
#include "vw.h"

using namespace LEARNER;

namespace CB
{
  char* bufread_label(CB::label* ld, char* c, io_buf& cache)
  {
    size_t num = *(size_t *)c;
    ld->costs.erase();
    c += sizeof(size_t);
    size_t total = sizeof(cb_class)*num;
    if (buf_read(cache, c, total) < total) 
      {
        cout << "error in demarshal of cost data" << endl;
        return c;
      }
    for (size_t i = 0; i<num; i++)
      {
        cb_class temp = *(cb_class *)c;
        c += sizeof(cb_class);
        ld->costs.push_back(temp);
      }
  
    return c;
  }

  size_t read_cached_label(shared_data*, void* v, io_buf& cache)
  {
    CB::label* ld = (CB::label*) v;
    ld->costs.erase();
    char *c;
    size_t total = sizeof(size_t);
    if (buf_read(cache, c, total) < total) 
      return 0;
    c = bufread_label(ld,c, cache);
  
    return total;
  }

  float weight(void* v)
  {
    return 1.;
  }

  char* bufcache_label(CB::label* ld, char* c)
  {
    *(size_t *)c = ld->costs.size();
    c += sizeof(size_t);
    for (size_t i = 0; i< ld->costs.size(); i++)
      {
        *(cb_class *)c = ld->costs[i];
        c += sizeof(cb_class);
      }
    return c;
  }

  void cache_label(void* v, io_buf& cache)
  {
    char *c;
    CB::label* ld = (CB::label*) v;
    buf_write(cache, c, sizeof(size_t)+sizeof(cb_class)*ld->costs.size());
    bufcache_label(ld,c);
  }

  void default_label(void* v)
  {
    CB::label* ld = (CB::label*) v;
    ld->costs.erase();
  }

  void delete_label(void* v)
  {
    CB::label* ld = (CB::label*)v;
    ld->costs.delete_v();
  }

  void copy_label(void*dst, void*src)
  {
    CB::label* ldD = (CB::label*)dst;
    CB::label* ldS = (CB::label*)src;
    copy_array(ldD->costs, ldS->costs);
  }

  bool substring_eq(substring ss, const char* str) {
    size_t len_ss  = ss.end - ss.begin;
    size_t len_str = strlen(str);
    if (len_ss != len_str) return false;
    return (strncmp(ss.begin, str, len_ss) == 0);
  }

  void parse_label(parser* p, shared_data* sd, void* v, v_array<substring>& words)
  {
    CB::label* ld = (CB::label*)v;

    for (size_t i = 0; i < words.size(); i++)
      {
        cb_class f;
	tokenize(':', words[i], p->parse_name);

        if( p->parse_name.size() < 1 || p->parse_name.size() > 3 )
        {
          cerr << "malformed cost specification!" << endl;
	  cerr << "terminating." << endl;
          throw exception();
        }

        f.partial_prediction = 0.;
        f.action = (uint32_t)hashstring(p->parse_name[0], 0);
        f.cost = FLT_MAX;

	if(p->parse_name.size() > 1)
	  f.cost = float_of_substring(p->parse_name[1]);
	
        if ( nanpattern(f.cost))
        {
	  cerr << "error NaN cost for action: ";
	  cerr.write(p->parse_name[0].begin, p->parse_name[0].end - p->parse_name[0].begin);
	  cerr << " terminating." << endl;
	  throw exception();
        }
      

        f.probability = .0;
        if(p->parse_name.size() > 2)
          f.probability = float_of_substring(p->parse_name[2]);
	
        if ( nanpattern(f.probability))
        {
	  cerr << "error NaN probability for action: ";
	  cerr.write(p->parse_name[0].begin, p->parse_name[0].end - p->parse_name[0].begin);
	  cerr << " terminating." << endl;
	  throw exception();
        }
        
        if( f.probability > 1.0 )
        {
          cerr << "invalid probability > 1 specified for an action, resetting to 1." << endl;
          f.probability = 1.0;
        }
        if( f.probability < 0.0 )
	  {
	    cerr << "invalid probability < 0 specified for an action, resetting to 0." << endl;
	    f.probability = .0;
	  }
	if (substring_eq(p->parse_name[0], "shared")) {
          if (p->parse_name.size() == 1) {
            f.probability = -1.f;
          } else
            cerr << "shared feature vectors should not have costs" << endl;
	}
	
        ld->costs.push_back(f);
      }
  }

  label_parser cb_label = {default_label, parse_label, 
				  cache_label, read_cached_label, 
				  delete_label, weight, 
				  copy_label,
				  sizeof(label)};

  bool example_is_test(example& ec)
  {
    v_array<CB::cb_class> costs = ec.l.cb.costs;
    if (costs.size() == 0) return true;
    for (size_t j=0; j<costs.size(); j++)
      if (costs[j].cost != FLT_MAX) return false;
    return true;    
  }

  bool ec_is_example_header(example& ec)  // example headers look like "0:-1" or just "shared"
  {
    v_array<CB::cb_class> costs = ec.l.cb.costs;
    if (costs.size() != 1) return false;
    if (costs[0].action != 0) return false;
    if (costs[0].cost >= 0) return false;
    return true;    
  }

  void print_update(vw& all, bool is_test, example& ec, const v_array<example*>* ec_seq, bool multilabel)
  {
    if (all.sd->weighted_examples >= all.sd->dump_interval && !all.quiet && !all.bfgs)
      {
	size_t num_current_features = ec.num_features;
	// for csoaa_ldf we want features from the whole (multiline example),
        // not only from one line (the first one) represented by ec
        if (ec_seq != nullptr)
	  {
	    num_current_features = 0;
          // If the first example is "shared", don't include its features.
          // These should be already included in each example (TODO: including quadratic and cubic).
          // TODO: code duplication csoaa.cc LabelDict::ec_is_example_header
	    example** ecc=ec_seq->begin;
	    example& first_ex = **ecc;
	    
	    v_array<CB::cb_class> costs = first_ex.l.cb.costs;
	    if (costs.size() == 1 && costs[0].action == 0 && costs[0].cost < 0) ecc++;
	    
	    for (; ecc!=ec_seq->end; ecc++)
	      num_current_features += (*ecc)->num_features;
	  }
	
	std::string label_buf;
        if (is_test)
          label_buf = " unknown";
        else
          label_buf = " known";
	
	if (multilabel) {
	  std::ostringstream pred_buf;
	  
	  pred_buf << std::setw(all.sd->col_current_predict) << std::right << std::setfill(' ')
		   << ec.pred.multilabels.label_v[0]<<".....";			
	  all.sd->print_update(all.holdout_set_off, all.current_pass, label_buf, pred_buf.str(), 
			       num_current_features, all.progress_add, all.progress_arg);;
	}
	else
	  all.sd->print_update(all.holdout_set_off, all.current_pass, label_buf, ec.pred.multiclass, 
			     num_current_features, all.progress_add, all.progress_arg);
      }
  }
}

namespace CB_EVAL
{
  size_t read_cached_label(shared_data*sd, void* v, io_buf& cache)
  {
    CB_EVAL::label* ld = (CB_EVAL::label*) v;
    char* c;
    size_t total = sizeof(uint32_t);
    if (buf_read(cache, c, total) < total) 
      return 0;
    ld->action = *(uint32_t*)c;
    c += sizeof(uint32_t);
    
    return total + CB::read_cached_label(sd, &(ld->event), cache);
  }

  void cache_label(void* v, io_buf& cache)
  {
    char *c;
    CB_EVAL::label* ld = (CB_EVAL::label*) v;
    buf_write(cache, c, sizeof(uint32_t));
    *(uint32_t *)c = ld->action;
    c+= sizeof(uint32_t);
    
    CB::cache_label(&(ld->event), cache);
  }

  void default_label(void* v)
  {
    CB_EVAL::label* ld = (CB_EVAL::label*) v;
    CB::default_label(&(ld->event));
    ld->action = 0;
  }

  void delete_label(void* v)
  {
    CB_EVAL::label* ld = (CB_EVAL::label*)v;
    CB::delete_label(&(ld->event));
  }

  void copy_label(void*dst, void*src)
  {
    CB_EVAL::label* ldD = (CB_EVAL::label*)dst;
    CB_EVAL::label* ldS = (CB_EVAL::label*)src;
    CB::copy_label(&(ldD->event), &(ldS)->event);
    ldD->action = ldS->action;
  }

  void parse_label(parser* p, shared_data* sd, void* v, v_array<substring>& words)
  {
    CB_EVAL::label* ld = (CB_EVAL::label*)v;
    
    if (words.size() < 2)
      {
	cout << "Evaluation can not happen without an action and an exploration" << endl;
	throw exception();
      }
    
    ld->action = (uint32_t)hashstring(words[0], 0);    
    
    words.begin++;
    
    CB::parse_label(p, sd, &(ld->event), words);
    
    words.begin--;
  }

  label_parser cb_eval = {default_label, parse_label, 
			  cache_label, read_cached_label, 
			  delete_label, CB::weight, 
			  copy_label,
			  sizeof(CB_EVAL::label)};
}
