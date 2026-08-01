#include "quotemesh/consolidated_book.hpp"

#include <bit>

namespace qm {

ConsolidatedBook::ConsolidatedBook(std::size_t reserve_symbols) { books_.reserve(reserve_symbols); }

ConsolidatedBook::SymbolBook& ConsolidatedBook::ensure(SymbolId sym) {
  if (sym >= books_.size()) books_.resize(static_cast<std::size_t>(sym) + 1);
  return books_[sym];
}

bool ConsolidatedBook::recompute(SymbolBook& b, Timestamp ts) {
  Nbbo n;
  // Only visit venues that actually quote this symbol (typically a handful of 32).
  for (std::uint32_t mask = b.active; mask != 0; mask &= mask - 1) {
    const int v = std::countr_zero(mask);
    const Quote& q = b.venues[static_cast<std::size_t>(v)];
    const std::uint32_t bit = 1u << v;
    if (q.bid_px != kNoBid) {
      if (q.bid_px > n.bid_px) {
        n.bid_px = q.bid_px; n.bid_sz = q.bid_sz; n.bid_venues = bit;
      } else if (q.bid_px == n.bid_px) {
        n.bid_sz += q.bid_sz; n.bid_venues |= bit;
      }
    }
    if (q.ask_px != kNoAsk) {
      if (q.ask_px < n.ask_px) {
        n.ask_px = q.ask_px; n.ask_sz = q.ask_sz; n.ask_venues = bit;
      } else if (q.ask_px == n.ask_px) {
        n.ask_sz += q.ask_sz; n.ask_venues |= bit;
      }
    }
  }
  if (n.same_market(b.nbbo)) return false;
  n.updated_ts = ts;
  b.nbbo = n;
  return true;
}

bool ConsolidatedBook::apply_quote(SymbolId sym, ExchangeId ex, const Quote& q, Timestamp ts) {
  if (ex >= kMaxExchanges) return false;
  SymbolBook& b = ensure(sym);
  Quote& slot = b.venues[ex];
  const std::uint32_t bit = 1u << ex;

  Quote nq = q;
  if (nq.bid_sz == 0) nq.bid_px = kNoBid;  // zero size means the side is withdrawn
  if (nq.ask_sz == 0) nq.ask_px = kNoAsk;

  const Quote old = slot;
  slot = nq;
  if (nq.bid_px == kNoBid && nq.ask_px == kNoAsk) b.active &= ~bit; else b.active |= bit;

  // Fast path: if this venue neither was nor becomes part of the NBBO, nothing can change.
  const Nbbo& cur = b.nbbo;
  const bool touched_before = (cur.bid_venues | cur.ask_venues) & bit;
  const bool improves_or_joins =
      (nq.bid_px != kNoBid && nq.bid_px >= cur.bid_px) ||
      (nq.ask_px != kNoAsk && nq.ask_px <= cur.ask_px);
  if (!touched_before && !improves_or_joins) return false;
  if (old.bid_px == nq.bid_px && old.ask_px == nq.ask_px && old.bid_sz == nq.bid_sz &&
      old.ask_sz == nq.ask_sz)
    return false;
  return recompute(b, ts);
}

void ConsolidatedBook::apply_trade(SymbolId sym, ExchangeId ex, const Trade& t, Timestamp ts) {
  SymbolBook& b = ensure(sym);
  b.last.px = t.px;
  b.last.sz = t.sz;
  b.last.exchange = ex;
  b.last.ts = ts;
  b.last.volume += t.sz;
}

std::vector<SymbolId> ConsolidatedBook::clear_venue(ExchangeId ex, Timestamp ts) {
  std::vector<SymbolId> changed;
  if (ex >= kMaxExchanges) return changed;
  const std::uint32_t bit = 1u << ex;
  for (std::size_t s = 0; s < books_.size(); ++s) {
    SymbolBook& b = books_[s];
    if (!(b.active & bit)) continue;
    b.venues[ex] = Quote{};
    b.active &= ~bit;
    if (recompute(b, ts)) changed.push_back(static_cast<SymbolId>(s));
  }
  return changed;
}

const Nbbo* ConsolidatedBook::nbbo(SymbolId sym) const noexcept {
  return sym < books_.size() ? &books_[sym].nbbo : nullptr;
}

const ConsolidatedBook::SymbolBook* ConsolidatedBook::book(SymbolId sym) const noexcept {
  return sym < books_.size() ? &books_[sym] : nullptr;
}

}  // namespace qm
