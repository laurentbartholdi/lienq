/******************************************************************************
**
**                 Nilpotent Quotient for Lie Rings
** operations.c                                                 Csaba Schneider
**                                                           schcs@math.klte.hu
*/

#include "nq.h"
#include <map>

vec_supply<hollowcvec> vecstack;

/* Lie algebra operations.
 *
 * By convention, these operations do not return a normalized vector,
 * i.e. with exponents in the range [0,pc.Exponent[g]. Normalization
 * is done at the end of a calculation, with liecollect.
 */

// this += [v,w]
void hollowcvec::liebracket(const pcpresentation &pc, const hollowcvec v, const hollowcvec w) {
  coeff c;
  coeff_init(c);

  for (const auto &kcv : v)
    for (const auto &kcw : w)
      if (kcv.first <= pc.NrPcGens && kcw.first <= pc.NrPcGens && pc.Generator[kcv.first].w + pc.Generator[kcw.first].w <= pc.Class) {
	coeff_mul(c, kcv.second, kcw.second);
        if (kcv.first > kcw.first)
	  addmul(c, pc.Comm[kcv.first][kcw.first]);
	else if (kcw.first > kcv.first)
	  submul(c, pc.Comm[kcw.first][kcv.first]);
      }
  coeff_clear(c);
}

// add/subtract [[a,b],c]. sign == add, ~sign == subtract.
void hollowcvec::lie3bracket(const pcpresentation &pc, gen a, gen b, gen c, bool sign) {
  const bool sab = (a < b);
  const sparsecvec v = sab ? pc.Comm[b][a] : pc.Comm[a][b];
  
  for (const auto &kc : v) {
    if (kc.first > pc.NrPcGens)
      break;
    if (kc.first == c)
      continue;
    const bool skc = kc.first < c;
    const sparsecvec w = skc ? pc.Comm[c][kc.first] : pc.Comm[kc.first][c];
    if (sign ^ sab ^ skc)
      addmul(kc.second, w);
    else
      submul(kc.second, w);
  }
}

#ifdef coeff_prime
static void frobenius_brackets(hollowcvec &r, const pcpresentation &pc, hollowcvec s[coeff_prime+1], coeff tempc[2], int c, unsigned i) {
  if (s[i].empty())
    return;
  if (i == coeff_prime) {
    coeff_set_si(tempc[0], c);
    coeff_inv(tempc[1], tempc[0]);
    r.addmul(tempc[1], s[i]);
  } else {
    s[i+1].liebracket(pc, s[i], s[0]);
    frobenius_brackets(r, pc, s, tempc, c+1, i+1);
    s[i+1].clear();
    s[i+1].liebracket(pc, s[i], s[1]);
    frobenius_brackets(r, pc, s, tempc, c, i+1);
    s[i+1].clear();
  }
}

// the p-power mapping, in restricted algebras
void hollowcvec::frobenius(const pcpresentation &pc, const hollowcvec v) {
  for (const auto &kc : v)
    addmul(kc.second, pc.Power[kc.first]); // formally, add kc.second^p * ...

  hollowcvec z[coeff_prime+1];
  for (unsigned i = 0; i <= coeff_prime; i++)
    z[i] = vecstack.fresh();
  z[1].copy(v);

  coeff c[2];
  coeff_init(c[0]);
  coeff_init(c[1]);
  
  for (const auto &kc : v) {
    coeff_zero(z[1][kc.first]);
    coeff_set(z[0][kc.first], kc.second);
    z[2].liebracket(pc, z[0], z[1]);
    frobenius_brackets(*this, pc, z, c, 1, 2);
    z[0].clear();
    z[2].clear();
  }

  coeff_clear(c[1]);
  coeff_clear(c[0]);
  for (int i = coeff_prime; i >= 0; i--)
    vecstack.release(z[i]);
}
#endif

void hollowcvec::liecollect(const pcpresentation &pc) {
  if (pc.Jennings) {
    /* special case: here there is no torsion, but the pc.Power field is
       used for the p-mapping */
    for (const auto &kc : *this)
      if (!coeff_reduced_p(kc.second, pc.Exponent[kc.first]))
	coeff_fdiv_r((*this)[kc.first], kc.second, pc.Exponent[kc.first]);
    return;
  }
  
  coeff q;
  coeff_init(q);
  
  for (const auto &kc : *this)
    if (!coeff_reduced_p(kc.second, pc.Exponent[kc.first])) {
      coeff_fdiv_qr(q, (*this)[kc.first], kc.second, pc.Exponent[kc.first]);
      addmul(q, pc.Power[kc.first]);
    }
  coeff_clear(q);
}

/* group operations */

/****************************************************************
 * a simple stack for the collector
 */
typedef T_stack<coeff> coeff_stack;
coeff_stack collectstack;

/****************************************************************
 * a cache for conjugates. we throw away the cache after every single
 * collection step of a fixed g^c moving across a sequence of various
 * h^d.

 * the cache stores in entry {i,k,l} the conjugate g_i^(g^(p^k 2^l)).
 */
struct conjdict_entry {
  gen g;
  unsigned two_pow, p_pow;
};
// special value, used to store g_i^(g^|c|)
const unsigned TOP_POW = -1;
constexpr conjdict_entry top_conj_entry(gen g) {
  return {.g = g, .two_pow = TOP_POW, .p_pow = TOP_POW};
}
typedef std::unordered_map<conjdict_entry,sparsecvec> conjdict_map;

template<> struct std::hash<conjdict_entry> {
  size_t operator()(const conjdict_entry &key) const {
    return (key.g << 16) + (key.p_pow << 8) + key.two_pow;
  }
};

template<> struct std::equal_to<conjdict_entry> {
  bool operator()(const conjdict_entry &key1, const conjdict_entry &key2) const {
    return key1.g == key2.g && key1.p_pow == key2.p_pow && key1.two_pow == key2.two_pow;
  }   
};

#ifdef coeff_prime
#define pow_base coeff_prime
#else
#define pow_base 2
#endif

// the following functions compute g^(h^c) using cache. assume g > h.

// return k.g^(h^(2^k.two_pow p^p_pow)), computing it if needed, and put it in the cache.
static sparsecvec conj_lookup(const pcpresentation &pc, gen h, const coeff &c, const conjdict_entry &k, conjdict_map &conjdict) {
  auto f = conjdict.find(k);
  if (f != conjdict.end())
    return f->second;

  // OK, so it's not yet in the table. Let's fill it in.
  hollowcvec v = vecstack.fresh();

  if (k.two_pow == TOP_POW && k.p_pow == TOP_POW) { // return g^(h^|c|)
    coeff d;
    coeff_init(d);
    if (coeff_cmp_si(c, 0) < 0)
      coeff_neg(d, c);
    else
      coeff_set(d, c);

    coeff_set_si(v[k.g], 1);
    for (unsigned k = 0; coeff_nz_p(d); k++) {
      unsigned long r = coeff_fdiv_q_ui(d, d, pow_base);
      for (unsigned l = 0; r != 0; l++, r >>= 1)
	if (r & 1) {
	  hollowcvec x = vecstack.fresh();
	  for (const auto &kc : v)
	    x.pow(pc, conj_lookup(pc, h, c, {.g = kc.first, .two_pow = l, .p_pow = k}, conjdict), kc.second);
	  v.copy(x);
	  vecstack.release(x);
	}
    }
    coeff_clear(d);
  } else if (k.two_pow > 0) { // return g^(h^(2m)) = (g^(h^m))^(h^m)
    // conj[g,k,l+1] = conj[g,k,l]@conj[*,k,l]    
    for (const auto &kc : conj_lookup(pc, h, c, {.g = k.g, .two_pow = k.two_pow-1, .p_pow = k.p_pow}, conjdict))
      v.pow(pc, conj_lookup(pc, h, c, {.g = kc.first, .two_pow = k.two_pow-1, .p_pow = k.p_pow}, conjdict), kc.second);
  } else if (k.p_pow > 0) { // return g^(h^(p^m))
    // conj[g,k+1,0] = g @ conj[g,k,0]^a0 @ ... @ conj[g,k,l]^al
    coeff_set_si(v[k.g], 1);
    unsigned long p = pow_base;
    for (unsigned l = 0; p != 0; l++, p >>= 1)
      if (p & 1) {
	hollowcvec x = vecstack.fresh();
	for (const auto &kc : v)
	  x.pow(pc, conj_lookup(pc, h, c, {.g = kc.first, .two_pow = l, .p_pow = k.p_pow-1}, conjdict), kc.second);
	v.copy(x);
	vecstack.release(x);
      }
  } else { // two_pow = p_pow = 0. return g^h = g*[g,h]
    if (pc.Generator[k.g].w < pc.Class)
      v.copy(pc.Comm[k.g][h]);
    coeff_set_si(v[k.g], 1);
  }

  sparsecvec &result = conjdict[k];
  result.alloc(v.size());
  result.copy(v);
  vecstack.release(v);
  return result;
}

// this is the main function, computing g^(h^c), returned in a fresh vector.
// we can assume that, throughout a run, h and c remain constant.
hollowcvec Conjugate(const pcpresentation &pc, gen g, gen h, const coeff *c, conjdict_map &conjdict) {
  hollowcvec v = vecstack.fresh();

  if (c == nullptr) { // speedup, c==nullptr means "1"
    if (pc.Generator[g].w < pc.Class)
      v.copy(pc.Comm[g][h]);
    coeff_set_si(v[g], 1);
    return v;
  }
  
  if (coeff_cmp_si(*c, 0) > 0) {
    v.copy(conj_lookup(pc, h, *c, top_conj_entry(g), conjdict));
  } else {
    /* Let us write h^c = x. Then we know all g_i^x, but we want
       g^(x^-1).  For this, we set w = g^x, v = g; and repeatedly we
       find a term beyond the inital g in w, and cancel it in w with a
       g_i^x, performing the same operation on v with a g_i.  In the
       end, w = g and v = g^(x^-1).
    */
    hollowcvec w = vecstack.fresh();
    w.copy(conj_lookup(pc, h, *c, top_conj_entry(g), conjdict));
    coeff_set_si(v[g], 1);

    for (auto p = w.upper_bound(g); p != w.end(); p++) {
      coeff_neg(v[p->first], p->second);
      if (!coeff_reduced_p(v[p->first], pc.Exponent[p->first]))
	coeff_add(v[p->first], v[p->first], pc.Exponent[p->first]);
      w.pow(pc, conj_lookup(pc, h, *c, top_conj_entry(p->first), conjdict), v[p->first]);
    }
    
    vecstack.release(w);
  }

  if (Debug >= 3) {
    fprintf(LogFile, "Conjugate: a%d^(a%d^" PRIcoeff ") = " PRIhollowcvec "\n", g, h, c, &v);
  }
  return v;
}

// for speedups, we allow c to be nullptr, which means really "1"
void hollowcvec::collect(const pcpresentation &pc, gen g, const coeff *c) {
  if (Debug >= 3) {
    fprintf(LogFile, "[collect (" PRIhollowcvec ") * a%d", this, g);
    if (c != nullptr)
      fprintf(LogFile, "^" PRIcoeff, c);
  }
    
  bool reducecomm = false;
  // first, check if the insertion will be easy
  const auto lastnoncommuting = --upper_bound(pc.LastGen[pc.Class-pc.Generator[g].w]);

  for (auto p = lastnoncommuting; p != end() && p->first > g; p--)
    if (!pc.Comm[p->first][g].empty()) {
      reducecomm = true;
      break;
    }

  if (c == nullptr)
    coeff_add_si((*this)[g], (*this)[g], 1);
  else
    coeff_add((*this)[g], (*this)[g], *c);
  bool reducepower = !coeff_reduced_p((*this)[g], pc.Exponent[g]);

  if (reducecomm || reducepower) {
    // find the generators that we have to skip over, put them in storage
    hollowcvec storage = vecstack.fresh(); // @@@ could be faster with sparsecvec

    for (auto p = lastnoncommuting; p != end() && p->first > g; p--) {
      coeff_set(storage[p->first], p->second);
      coeff_zero(p->second);
    }

    if (reducepower) {
      coeff_sub((*this)[g], (*this)[g], pc.Exponent[g]);
      mul(pc, pc.Power[g]);
    }
  
    if (reducecomm) {
      // now begins the hard work. push back the conjugate of storage by g^c.
      conjdict_map conjdict; // @@@ performance: this cache could be persistent
      conjdict.reserve(10*pc.NrPcGens); // @@@ random factor
      
      for (const auto &kc : storage) {
	if (pc.Comm[kc.first][g].empty())
	  mul(pc, kc.first, kc.second);
	else {
	  hollowcvec conj = Conjugate(pc, kc.first, g, c, conjdict);
	  pow(pc, conj, kc.second);
	  vecstack.release(conj);
	}
      }
      for (auto &p : conjdict)
	p.second.free();
    } else
      mul(pc, storage);

    vecstack.release(storage);
  }

  if (Debug >= 3) {
    fprintf(LogFile, " = " PRIhollowcvec "]\n", this);
  }
}

#if 0
// a simple collector, for debugging; also written recursively.
struct simplestackslot {
  sparsecvec v;
  int e;
};

void SimpleCollect(const pcpresentation &pc, hollowcvec &lhs, const sparsecvec &rhs, int e) {
  if (e < 0) {
    hollowcvec i = vecstack.fresh();
    i.div(pc, rhs);
    sparsecvec s = i.getsparse();
    vecstack.release(i);
    SimpleCollect(pc, lhs, s, -e);
    s.free();
    return;
  }

  while (e--)
    for (const auto &kc : rhs) {
      int e = coeff_get_si(kc.second);
      int deltae = (e > 0) - (e < 0);

      for (; e != 0; e -= deltae) {
	std::vector<simplestackslot> collectstack;
	// move kc.first^deltae to its correct position in lhs
      
	for (auto p = --lhs.end(); p != lhs.end() && p->first > kc.first; p--)
	  if (pc.Generator[p->first].w+pc.Generator[kc.first].w <= pc.Class) {
	    // push [(p->first)^(kc.first^deltae)]^p.second
	    simplestackslot s;
	    if (deltae == 1) {
	      s.v.alloc(pc.Comm[p->first][kc.first].size()+1);
	      s.v.front().first = p->first;
	      coeff_set_si(s.v.front().second, 1);
	      s.v.window(1).copy(pc.Comm[p->first][kc.first]);
	    } else {
	      unsigned len = pc.Comm[p->first][kc.first].size()+3;
	      s.v.alloc(len);
	      s.v[0].first = p->first;
	      coeff_set_si(s.v[0].second, 1);
	      s.v[1].first = kc.first;
	      coeff_set_si(s.v[1].second, 1);
	      for (unsigned i = 0; i < len; i++) {
		s.v[i+2].first = pc.Comm[p->first][kc.first][len-1-i].first;
		coeff_neg(s.v[i+2].second, pc.Comm[p->first][kc.first][len-1-i].second);
	      }
	      s.v[len+2].first = kc.first;
	      coeff_set_si(s.v[len+2].second, -1);
	      s.v.truncate(len+3);
	    }
	    s.e = coeff_get_si(p->second);
	    collectstack.push_back(s);

	    coeff_zero(p->second);
	  }
	coeff_add_si(lhs[kc.first], lhs[kc.first], deltae);
	if (!coeff_reduced_p(lhs[kc.first], pc.Exponent[kc.first])) {
	  coeff q;
	  coeff_init(q);
	  coeff_fdiv_qr(q, lhs[kc.first], lhs[kc.first], pc.Exponent[kc.first]);
	  simplestackslot s;
	  s.v.alloc(pc.Power[kc.first].size());
	  s.v.copy(pc.Power[kc.first]);
	  s.e = coeff_get_si(q);
	  coeff_clear(q);
	  collectstack.push_back(s);
	}
	while (!collectstack.empty()) {
	  SimpleCollect(pc, lhs, collectstack.back().v, collectstack.back().e);
	  collectstack.back().v.free();
	  collectstack.pop_back();
	}
      }
    }
}
#endif

// this *= v^-1*w. makes v=w in the process.
void hollowcvec::lquo(const pcpresentation &pc, hollowcvec v, const hollowcvec w) {
  /* we seek u with v u = w.
     say v = g^c * v', w = g^d * w'; then u = g^e * u' with e == d-c, and
     w = vu = v g^e u'; so we seek u' with (v g^e) u' = w, knowing that
     (v g^e) starts with g^d which we can skip.
  */
  
  auto pv = v.begin();
  auto pw = w.begin();
  coeff c;
  coeff_init(c);

  for (;;) {
    gen g = LASTGEN;
    bool minatv, minatw;
    if ((minatv = (pv != v.end()))) g = pv->first;
    if ((minatw = (pw != w.end()))) {
      if (pw->first > g) minatw = false;
      if (pw->first < g) g = pw->first, minatv = false;
    }
    if (minatv && minatw)
      coeff_sub(c, (pw++)->second, pv->second);
    else if (minatv)
      coeff_neg(c, pv->second);
    else if (minatw)
      coeff_set(c, (pw++)->second);
    else
      break;
    if (!coeff_reduced_p(c, pc.Exponent[g]))
      coeff_add(c, c, pc.Exponent[g]);
    mul(pc, g, c);
    v.mul(pc, g, c);
    pv = v.upper_bound(g);
  }

  coeff_clear(c);
}

// this *= v^-1.
template <typename V> void hollowcvec::div(const pcpresentation &pc, const V v) {
  /* we seek u with v u = 1.
     say v = g^c * v'; then u = g^d * u' with d == -c, and we have
     1 = vu = v g^d u'; so we seek u' with (v g^d) u' = 1, knowing that
     (v g^d) starts with a monomial >g.
  */
  coeff c;
  coeff_init(c);
  hollowcvec w = vecstack.fresh();
  w.copy(v);
  
  for (const auto &kc : w) {
    gen g = kc.first;
    coeff_neg (c, kc.second);
    if (!coeff_reduced_p(c, pc.Exponent[g]))
      coeff_add(c, c, pc.Exponent[g]);
    mul(pc, g, c);
    w.mul(pc, g, c);
  }
  vecstack.release(w);
  coeff_clear(c);
}

// this *= v^c
template <typename V> void hollowcvec::pow(const pcpresentation &pc, const V v, const coeff &c) {
  if (coeff_z_p(c)) // easy peasy
    return;
  
  coeff d;
  coeff_init(d);
  hollowcvec x = vecstack.fresh();

  if (coeff_sgn(c) < 0) {
    coeff_neg(d, c);
    x.div(pc, v);
  } else {
    coeff_set(d, c);
    x.copy(v);
  }

  bool next_nonzero = true;
  while (next_nonzero) {
    unsigned long r = coeff_fdiv_q_ui(d, d, pow_base), l = 1;
    hollowcvec w = vecstack.fresh();
    next_nonzero = coeff_nz_p(d);
    for (;;) {
      // throughout this loop, l is a power of 2; x = v^(p^i*l); and
      // at the end w = x = v^(p^(i+1))
      if (r & l)
	mul(pc, x);
      if (next_nonzero && (pow_base & l))
	w.mul(pc, x);
      l <<= 1;
      if (l > pow_base)
	break;
      if (next_nonzero) {
	hollowcvec y = vecstack.fresh();
	y.copy(x);
	x.mul(pc, y); // we can't multiply directly with x, operands must be !=
	vecstack.release(y);
      }
    }
    x.copy(w);
    vecstack.release(w);
  }

  coeff_clear(d);
  vecstack.release(x);
}

/* evaluate relator, given as tree */
void hollowcvec::eval(const pcpresentation &pc, node *rel) {
  switch (rel->type) {
  case TSUM:
    {
      eval(pc, rel->l);
      hollowcvec t = vecstack.fresh();
      t.eval(pc, rel->r);
      add(t);
      vecstack.release(t);
    }
    break;
  case TPROD:
    if (rel->l->type == TNUM) {
      eval(pc, rel->r);
      mul(rel->l->n);
    } else {
      eval(pc, rel->l);
      hollowcvec t = vecstack.fresh();
      t.eval(pc, rel->r);
      mul(pc, t);
      vecstack.release(t);
    }
    break;
  case TQUO:
    {
      eval(pc, rel->l);
      hollowcvec t = vecstack.fresh();
      t.eval(pc, rel->r);
      div(pc, t);
      vecstack.release(t);
    }
    break;
  case TLQUO:
    {
      hollowcvec t = vecstack.fresh();
      hollowcvec u = vecstack.fresh();
      t.eval(pc, rel->l);
      u.eval(pc, rel->r);
      lquo(pc, t, u);
      vecstack.release(u);
      vecstack.release(t);
    }
    break;
  case TBRACK:
    {
      hollowcvec t = vecstack.fresh();
      hollowcvec u = vecstack.fresh();
      t.eval(pc, rel->l);
      u.eval(pc, rel->r);
#ifdef LIEALG
      liebracket(pc, t, u);
#else
      hollowcvec v = vecstack.fresh();
      v.copy(t);
      v.mul(pc, u); // v = t*u
      u.mul(pc, t); // u = u*t
      lquo(pc, u, v); // this = (u*t) \ (t*u)
      vecstack.release(v);
#endif      
      vecstack.release(u);
      vecstack.release(t);
    }
    break;
  case TGEN:
    copy(pc.Epimorphism[rel->g]);
    break;
  case TNEG:
    eval(pc, rel->u);
    neg();
    break;
  case TINV:
    {
      hollowcvec t = vecstack.fresh();
      t.eval(pc, rel->u);
      div(pc, t);
      vecstack.release(t);
    }
    break;
#ifdef coeff_prime
  case TFROB:
    {
      hollowcvec t = vecstack.fresh();
      t.eval(pc, rel->u);
      frobenius(pc, t);
      vecstack.release(t);
    }
    break;
#endif
  case TDIFF:
    {
      eval(pc, rel->l);
      hollowcvec t = vecstack.fresh();
      t.eval(pc, rel->r);
      sub(t);
      vecstack.release(t);
    }
    break;
  case TPOW:
    if (rel->r->type == TNUM) {
      hollowcvec t = vecstack.fresh();
      t.eval(pc, rel->l);
      pow(pc, t, rel->r->n);
      vecstack.release(t);
    } else {
      hollowcvec t = vecstack.fresh();
      hollowcvec u = vecstack.fresh();
      t.eval(pc, rel->l);
      u.eval(pc, rel->r);
      t.mul(pc, u);
      lquo(pc, u, t);
      vecstack.release(u);
      vecstack.release(t);
      break;
    }
    break;
  default:
    abortprintf(3, "eval: operator of type %s should not occur", nodename[rel->type]);
  }
}
