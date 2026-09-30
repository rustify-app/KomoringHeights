/**
 * @file tsume_workbench.hpp
 * @brief Exhaustive tsume verification and machine-readable reporting.
 *
 * This is deliberately separate from KomoringHeights' df-pn solver.  df-pn is
 * the fast proof engine; this class enumerates the bounded AND/OR tree needed
 * for composition checks (shorter mates, alternative attacks, and every
 * longest defence).
 */
#ifndef KOMORI_TSUME_WORKBENCH_HPP_
#define KOMORI_TSUME_WORKBENCH_HPP_

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "move_picker.hpp"
#include "node.hpp"
#include "../../usi.h"

namespace komori::tsume {

enum class Proof { kMate, kNoMate, kUnknown, kRepetition };

struct Line {
  std::vector<Move> moves;
  int mate_ply{-1};
};

struct Branch {
  Move move{MOVE_NONE};
  Proof proof{Proof::kUnknown};
  int mate_ply{-1};
  std::vector<Move> principal;
};

struct VerifyOptions {
  int max_ply{31};
  std::uint64_t max_nodes{5'000'000};
  bool double_king{false};
  std::vector<std::string> intended_usi;
};

struct VerifyResult {
  Proof proof{Proof::kUnknown};
  int mate_ply{-1};
  std::uint64_t nodes{0};
  bool complete{false};
  bool unique{false};
  bool alternative_mate{false};
  bool shorter_mate{false};
  bool no_mate_branch{false};
  bool repetition{false};
  bool illegal_intended{false};
  bool intended_mismatch{false};
  bool symmetric_solutions_collapsed{false};
  bool piece_surplus{false};
  bool unnecessary_piece{false};
  bool futile_interposition{false};
  bool prohibited_move{false};
  int hendou_count{0};  // 変同数: 受け方に同手数の最善応手が複数あったノード数の合計
  bool line_alternative{false};  // 作意途中（最終手を除く攻方手番）に別の詰み手がある
  std::vector<std::pair<int, Branch>> line_alternatives;  // (手目 0-origin, 別の詰み手)
  std::vector<std::string> unnecessary_squares;
  std::vector<Move> principal;
  std::vector<Branch> attacks;
  std::vector<Line> longest_lines;
  std::vector<std::string> reasons;
};

struct AestheticScores {
  double legalityScore{0}, uniquenessScore{0}, lengthScore{0}, complexityScore{0};
  double sacrificeScore{0}, techniqueScore{0}, economyScore{0}, visualScore{0};
  double originalityScore{0}, aestheticScore{0}, totalScore{0};
  // Raw X-variable values (Hirose 1998) for regression dataset export
  double x5_spread{0};    // X5: 配置の広さ (bounding box, 0-100)
  int    x6_count{0};     // X6: 盤上駒数
  double x7_weighted{0};  // X7: 駒種重み付き平均 (0-100)
  double x8_ratio{0};     // X8: 駒種数/駒数比率 (0-100)
  double x10_difficulty{0}; // X10: 探索局面数（難解さ）(0-100)
};

struct TechniqueSummary {
  int sacrifices{0}, major_sacrifices{0}, consecutive_sacrifices{0};
  int drops{0}, promotions{0}, non_promotions{0}, discovered_checks{0}, double_checks{0};
  int attacker_moves{0}, contact_checks{0}, defender_moves{0}, defender_king_moves{0};
  int non_king_defences{0};
  int captures_by_attacker{0};  // X3: 攻め方の駒取り数 (Hirose 1998, negative coefficient)
  int king_openness{0};         // X4: 詰み局面での玉の開放度 (Hirose 1998, positive) — computed at final position
  double piece_utilization{0};  // X9: 駒の使用率 (Hirose 1998, positive)
  int king_final_edge_dist{0};  // X12: 詰み局面での玉の辺からの距離 (Hirose 1998, positive)
  double chase_ratio{0};
  bool chasing_mate{false};
  std::vector<std::string> names;
};

struct WorkRecord {
  std::string id, canonical_id, parent_id, sfen, normalized_sfen;
  std::string title, author, notes, generated_at, parent_diff;
  std::string attacker_hand, defender_hand;
  std::string extension_reason;
  double parent_similarity{0};
  int parent_mate_ply{-1};
  bool double_king{false}, perfect{false};
  VerifyOptions conditions;
  VerifyResult verification;
  AestheticScores scores;
  TechniqueSummary techniques;
  bool aesthetic_pass{false};
  std::vector<std::string> aesthetic_reasons;
};

namespace detail {

struct Eval {
  Proof proof{Proof::kUnknown};
  int len{-1};
  std::vector<Move> pv;
  std::vector<Line> longest;
  bool futile_interposition{false};
  int hendou_count{0};  // 変同数: ANDノードで同手数複数応手が存在した回数の累積
};

struct CacheKey {
  Key key{};
  int remain{};
  bool operator==(const CacheKey& rhs) const { return key == rhs.key && remain == rhs.remain; }
};
struct CacheHash {
  std::size_t operator()(const CacheKey& k) const {
    return static_cast<std::size_t>(k.key ^ (static_cast<Key>(k.remain) * 0x9e3779b97f4a7c15ULL));
  }
};

inline std::string EscapeJson(const std::string& s) {
  std::ostringstream out;
  for (unsigned char c : s) {
    switch (c) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c);
        else out << static_cast<char>(c);
    }
  }
  return out.str();
}

inline const char* ProofName(Proof p) {
  switch (p) {
    case Proof::kMate: return "mate";
    case Proof::kNoMate: return "nomate";
    case Proof::kRepetition: return "repetition";
    default: return "unverified";
  }
}

inline std::string UsiLine(const std::vector<Move>& line) {
  std::ostringstream out;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (i) out << ' ';
    out << USI::move(line[i]);
  }
  return out.str();
}

inline std::string HumanLine(Position& pos, const std::vector<Move>& line) {
  std::ostringstream out;
  std::vector<StateInfo> states(line.size());
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (i) out << ' ';
    const std::string usi = USI::move(line[i]);
    const PieceType pt = raw_type_of(pos.moved_piece_before(line[i]));
    const char* name = pt == PAWN ? "P" : pt == LANCE ? "L" : pt == KNIGHT ? "N" :
                       pt == SILVER ? "S" : pt == GOLD ? "G" : pt == BISHOP ? "B" :
                       pt == ROOK ? "R" : pt == KING ? "K" : "?";
    out << (i + 1) << ':' << name << ' ';
    if (is_drop(line[i])) out << "drop->" << usi.substr(2, 2);
    else out << usi.substr(0, 2) << "->" << usi.substr(2, 2)
             << (is_promote(line[i]) ? " promote" : "");
    pos.do_move(line[i], states[i]);
  }
  for (auto it = line.rbegin(); it != line.rend(); ++it) pos.undo_move(*it);
  return out.str();
}

inline std::string StableId(const std::string& value) {
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : value) { h ^= c; h *= 1099511628211ULL; }
  std::ostringstream out; out << std::hex << std::setw(16) << std::setfill('0') << h;
  return out.str();
}

inline std::string HandSfen(Hand hand, bool lower) {
  const std::array<std::pair<PieceType,char>, 7> order{{{ROOK,'R'},{BISHOP,'B'},{GOLD,'G'},
      {SILVER,'S'},{KNIGHT,'N'},{LANCE,'L'},{PAWN,'P'}}};
  std::string out;
  for (auto [pt, c] : order) { const int n = hand_count(hand, pt); if (!n) continue;
    if (n > 1) out += std::to_string(n); out += lower ? static_cast<char>(std::tolower(c)) : c; }
  return out.empty() ? "-" : out;
}

inline std::string NormalizeSfenHandText(const std::string& sfen) {
  std::istringstream in(sfen);
  std::string board, turn, hand, move_no;
  if (!(in >> board >> turn >> hand >> move_no) || hand == "-") return sfen;
  const std::string order = "RBGSNLPrbgsnlp";
  std::array<int, 14> counts{};
  for (std::size_t i = 0; i < hand.size();) {
    int n = 0;
    while (i < hand.size() && std::isdigit(static_cast<unsigned char>(hand[i])))
      n = n * 10 + (hand[i++] - '0');
    if (i >= hand.size()) return sfen;
    const auto at = order.find(hand[i++]);
    if (at == std::string::npos) return sfen;
    counts[at] += n == 0 ? 1 : n;
  }
  std::string normalized;
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (counts[i] == 0) continue;
    if (counts[i] > 1) normalized += std::to_string(counts[i]);
    normalized += order[i];
  }
  return board + " " + turn + " " + (normalized.empty() ? "-" : normalized) + " " + move_no;
}

inline bool IsMirrorSymmetric(const Position& pos) {
  for (int s = 0; s < SQ_NB; ++s) {
    const Square sq = static_cast<Square>(s);
    if (pos.piece_on(sq) != pos.piece_on(Mir(sq))) return false;
  }
  return true;
}

inline std::string MirrorUsi(std::string move) {
  auto mirror_file = [](char c) { return static_cast<char>('0' + (10 - (c - '0'))); };
  if (move.size() >= 4 && move[1] == '*') move[2] = mirror_file(move[2]);
  else if (move.size() >= 4) { move[0] = mirror_file(move[0]); move[2] = mirror_file(move[2]); }
  return move;
}

}  // namespace detail

class ExhaustiveVerifier {
 public:
  explicit ExhaustiveVerifier(VerifyOptions options) : options_{std::move(options)} {}

  VerifyResult Run(Position& pos) {
    result_ = {};
    table_.clear();
    Node root(pos, true);
    auto eval = Search(root, options_.max_ply);
    result_.proof = eval.proof;
    result_.mate_ply = eval.len;
    result_.principal = eval.pv;
    result_.longest_lines = eval.longest;
    result_.futile_interposition = eval.futile_interposition;
    result_.hendou_count = eval.hendou_count;
    result_.complete = eval.proof != Proof::kUnknown && !budget_hit_;

    // Root attacks are intentionally searched again only through cached values;
    // this records every attack, including refutations and alternatives.
    MovePicker picker(root);
    int shortest = std::numeric_limits<int>::max();
    int mating_attacks = 0;
    for (const auto& ext : picker) {
      const Move move = ext.move;
      detail::Eval child;
      if (root.IsRepetitionAfter(move)) child.proof = Proof::kRepetition;
      else { root.DoMove(move); child = Search(root, options_.max_ply - 1); root.UndoMove(); }
      Branch b{move, child.proof, child.len < 0 ? -1 : child.len + 1, {move}};
      b.principal.insert(b.principal.end(), child.pv.begin(), child.pv.end());
      result_.attacks.push_back(std::move(b));
      if (child.proof == Proof::kUnknown) result_.complete = false;
      if (child.proof == Proof::kMate) {
        shortest = std::min(shortest, child.len + 1);
      }
      if (child.proof == Proof::kRepetition) result_.repetition = true;
      result_.futile_interposition |= child.futile_interposition;
      if (child.proof == Proof::kNoMate) result_.no_mate_branch = true;
    }
    std::unordered_set<std::string> solution_classes;
    const bool mirror_symmetric = detail::IsMirrorSymmetric(pos);
    for (const auto& b : result_.attacks) if (b.proof == Proof::kMate && b.mate_ply == result_.mate_ply) {
      std::string move = USI::move(b.move);
      if (mirror_symmetric) move = std::min(move, detail::MirrorUsi(move));
      solution_classes.insert(std::move(move));
    }
    mating_attacks = static_cast<int>(solution_classes.size());
    result_.symmetric_solutions_collapsed = mirror_symmetric &&
        solution_classes.size() < static_cast<std::size_t>(std::count_if(result_.attacks.begin(), result_.attacks.end(),
            [&](const Branch& b) { return b.proof == Proof::kMate && b.mate_ply == result_.mate_ply; }));
    result_.alternative_mate = mating_attacks > 1;
    result_.shorter_mate = result_.mate_ply >= 0 && shortest < result_.mate_ply;
    result_.unique = result_.complete && result_.proof == Proof::kMate && mating_attacks == 1;
    if (result_.proof == Proof::kMate) CheckLineAlternatives(root);
    ValidateIntended(pos);
    if (!options_.intended_usi.empty() && result_.mate_ply >= 0 &&
        result_.mate_ply < static_cast<int>(options_.intended_usi.size())) result_.shorter_mate = true;
    AddReasons();
    return result_;
  }

 private:
  detail::Eval Search(Node& node, int remain) {
    ++result_.nodes;
    if (result_.nodes > options_.max_nodes) { budget_hit_ = true; return {}; }
    if (remain < 0) return {};

    const detail::CacheKey ck{node.GetKey(), remain};
    if (auto it = table_.find(ck); it != table_.end()) return it->second;

    MovePicker picker(node);
    if (!node.IsOrNode() && picker.empty()) {
      detail::Eval terminal{Proof::kMate, 0, {}, {Line{{}, 0}}, false};
      table_[ck] = terminal;
      return terminal;
    }
    if (node.IsOrNode() && picker.empty()) {
      detail::Eval fail{Proof::kNoMate, -1, {}, {}, false};
      table_[ck] = fail;
      return fail;
    }
    // A live node at the horizon is a disproof for the bounded "mate within
    // max_ply" question. This is exact for early-mate/alternative checks at
    // that bound; callers must raise max_ply to make a stronger claim.
    if (remain == 0) return {Proof::kNoMate, -1, {}, {}, false};

    detail::Eval best;
    bool saw_unknown = false;
    bool saw_repetition = false;
    if (node.IsOrNode()) {
      int min_len = std::numeric_limits<int>::max();
      for (const auto& ext : picker) {
        const Move m = ext.move;
        detail::Eval child;
        if (node.IsRepetitionAfter(m)) child.proof = Proof::kRepetition;
        else { node.DoMove(m); child = Search(node, remain - 1); node.UndoMove(); }
        if (child.proof == Proof::kMate && child.len + 1 < min_len) {
          min_len = child.len + 1;
          best = child;
          best.len = min_len;
          best.pv.insert(best.pv.begin(), m);
          best.longest.clear();
          for (auto line : child.longest) { line.moves.insert(line.moves.begin(), m); line.mate_ply++; best.longest.push_back(std::move(line)); }
        } else if (child.proof == Proof::kUnknown) saw_unknown = true;
        else if (child.proof == Proof::kRepetition) saw_repetition = true;
      }
      if (min_len != std::numeric_limits<int>::max()) best.proof = Proof::kMate;
      else if (saw_unknown) best.proof = Proof::kUnknown;
      else if (saw_repetition) best.proof = Proof::kRepetition;
      else best.proof = Proof::kNoMate;
    } else {
      int max_len = -1;
      int moves_at_max = 0;     // max_lenを達成する有効応手数（変同カウント用）
      int max_child_hendou = 0; // max_len達成子ノードの変同数の最大値
      bool refuted = false;
      int effective_replies = 0;
      std::vector<Line> longest;
      for (const auto& ext : picker) {
        const Move m = ext.move;
        detail::Eval child;
        if (node.IsRepetitionAfter(m)) child.proof = Proof::kRepetition;
        else { node.DoMove(m); child = Search(node, remain - 1); node.UndoMove(); }
        // A non-king interposition which is immediately captured for mate has
        // no effect other than adding two plies. Under tsume rules it is a
        // futile interposition and is excluded from the defender's replies.
        // Only a drop between the king and a ranged checker is an interposition;
        // captures of the checker and moved interpositions are real defences.
        const bool futile = is_drop(m) && IsInterposition(node.Pos(), to_sq(m)) &&
                            child.proof == Proof::kMate && child.len == 1 &&
                            !child.pv.empty() && !is_drop(child.pv.front()) &&
                            to_sq(child.pv.front()) == to_sq(m);
        if (futile) { best.futile_interposition = true; continue; }
        ++effective_replies;
        best.futile_interposition |= child.futile_interposition;
        if (child.proof == Proof::kNoMate || child.proof == Proof::kRepetition) {
          refuted = true;
          saw_repetition |= child.proof == Proof::kRepetition;
          if (best.pv.empty()) { best.pv = {m}; best.pv.insert(best.pv.end(), child.pv.begin(), child.pv.end()); }
        } else if (child.proof == Proof::kUnknown) {
          saw_unknown = true;
        } else {
          const int len = child.len + 1;
          if (len > max_len) {
            max_len = len; moves_at_max = 1; max_child_hendou = child.hendou_count;
            longest.clear(); best.pv = {m}; best.pv.insert(best.pv.end(), child.pv.begin(), child.pv.end());
          } else if (len == max_len) {
            ++moves_at_max;
            max_child_hendou = std::max(max_child_hendou, child.hendou_count);
          }
          if (len == max_len) for (auto line : child.longest) { line.moves.insert(line.moves.begin(), m); line.mate_ply++; longest.push_back(std::move(line)); }
        }
      }
      if (effective_replies == 0) {
        best.proof = Proof::kMate; best.len = 0; best.pv.clear();
        best.longest = {Line{{}, 0}};
        if (best.proof == Proof::kMate) table_[ck] = best;
        return best;
      }
      if (refuted) best.proof = saw_repetition ? Proof::kRepetition : Proof::kNoMate;
      else if (saw_unknown) best.proof = Proof::kUnknown;
      else {
        best.proof = Proof::kMate; best.len = max_len; best.longest = std::move(longest);
        // 変同数: このノードでの変同(moves_at_max-1) + 子ノードの変同
        best.hendou_count = std::max(0, moves_at_max - 1) + max_child_hendou;
      }
    }
    // Unknown and repetition are path-sensitive and must not be cached.
    if (best.proof == Proof::kMate || best.proof == Proof::kNoMate) table_[ck] = best;
    return best;
  }

  static bool IsInterposition(const Position& pos, Square to) {
    const Square ksq = pos.king_square(pos.side_to_move());
    Bitboard checkers = pos.checkers();
    while (checkers) {
      if (between_bb(ksq, checkers.pop()).test(to)) return true;
    }
    return false;
  }

  // 作意（principal）上の攻方手番で、最終手以外に別の詰み手があれば余詰として記録する。
  // OR ノードは全王手を探索済みなので、ここでの Search はほぼ置換表の参照で済む。
  void CheckLineAlternatives(Node& root) {
    const auto& line = result_.principal;
    std::size_t made = 0;
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
      if (i % 2 == 0 && i > 0) {
        const int remain = options_.max_ply - static_cast<int>(i);
        MovePicker picker(root);
        for (const auto& ext : picker) {
          const Move m = ext.move;
          if (m == line[i] || root.IsRepetitionAfter(m)) continue;
          root.DoMove(m);
          const auto child = Search(root, remain - 1);
          root.UndoMove();
          if (child.proof == Proof::kUnknown) result_.complete = false;
          if (child.proof != Proof::kMate) continue;
          Branch b{m, child.proof, child.len + 1, {m}};
          b.principal.insert(b.principal.end(), child.pv.begin(), child.pv.end());
          result_.line_alternatives.emplace_back(static_cast<int>(i), std::move(b));
          result_.line_alternative = true;
        }
      }
      root.DoMove(line[i]);
      ++made;
    }
    while (made-- > 0) root.UndoMove();
  }

  void ValidateIntended(Position& pos) {
    if (options_.intended_usi.empty()) return;
    std::vector<StateInfo> states(options_.intended_usi.size());
    std::vector<Move> made;
    std::vector<std::string> parsed;
    for (std::size_t i = 0; i < options_.intended_usi.size(); ++i) {
      Move m = USI::to_move(pos, options_.intended_usi[i]);
      if (m == MOVE_NONE || !pos.legal(m) || (i % 2 == 0 && !pos.gives_check(m))) {
        result_.illegal_intended = true;
        result_.prohibited_move = true;
        for (auto it = made.rbegin(); it != made.rend(); ++it) pos.undo_move(*it);
        return;
      }
      parsed.push_back(USI::move(m));
      pos.do_move(m, states[i]);
      made.push_back(m);
    }
    for (auto it = made.rbegin(); it != made.rend(); ++it) pos.undo_move(*it);
    std::vector<std::string> actual;
    for (Move m : result_.principal) actual.push_back(USI::move(m));
    result_.intended_mismatch = parsed != actual;
  }

  void AddReasons() {
    if (budget_hit_) result_.reasons.push_back("node limit reached; completeness checks are unverified");
    else if (!result_.complete) result_.reasons.push_back("the ply limit leaves at least one alternative attack unverified");
    if (result_.proof == Proof::kMate) result_.reasons.push_back("all replies on the principal AND branches are mated");
    if (result_.proof == Proof::kNoMate) result_.reasons.push_back("no forced mate exists within the requested ply bound");
    if (result_.alternative_mate) result_.reasons.push_back("multiple root checking moves force mate (alternative solution)");
    if (result_.line_alternative) result_.reasons.push_back("another checking move mates in the middle of the main line (alternative solution)");
    if (result_.symmetric_solutions_collapsed) result_.reasons.push_back("mirror-equivalent root moves were treated as one solution");
    if (result_.repetition) result_.reasons.push_back("a checking sequence repeats and is a failure for the attacker");
    if (result_.illegal_intended) result_.reasons.push_back("the supplied intended line contains an illegal or non-checking attack");
    if (result_.intended_mismatch) result_.reasons.push_back("the supplied intended line differs from the shortest/longest principal line");
  }

  VerifyOptions options_;
  VerifyResult result_;
  bool budget_hit_{false};
  std::unordered_map<detail::CacheKey, detail::Eval, detail::CacheHash> table_;
};

inline TechniqueSummary AnalyzeTechniques(Position& pos, const std::vector<Move>& line) {
  TechniqueSummary t;
  std::vector<StateInfo> states(line.size());
  uint8_t used_types_mask = 0;  // X9: bit i = piece type (i+1) used by attacker
  const Color attacker_color = pos.side_to_move();  // X2: 不成判定用
  bool previous_sacrifice = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const Move m = line[i];
    const Piece moved = pos.moved_piece_before(m);
    if (i % 2 == 0) {
      ++t.attacker_moves;
      if (is_drop(m)) ++t.drops;
      if (is_promote(m)) ++t.promotions;
      // X3: 攻め方の駒取り (非drop移動で行先に駒がある)
      if (!is_drop(m) && pos.piece_on(to_sq(m)) != NO_PIECE) ++t.captures_by_attacker;
      // X2: 不成 — 成れる駒が成り区域に入るのに成らない
      if (!is_drop(m) && !is_promote(m)) {
        const PieceType rpt = raw_type_of(moved);
        const bool can_promo = (rpt >= 1 && rpt <= 6);  // PAWN..ROOK (not GOLD, not KING)
        if (can_promo) {
          const int to_r  = static_cast<int>(rank_of(to_sq(m)));
          const int from_r = static_cast<int>(rank_of(from_sq(m)));
          const bool in_zone = (attacker_color == BLACK) ?
              (to_r <= 2 || from_r <= 2) : (to_r >= 6 || from_r >= 6);
          if (in_zone) ++t.non_promotions;
        }
      }
      // X9: 使用駒種を記録
      const int pt_idx = static_cast<int>(raw_type_of(moved));
      if (pt_idx >= 1 && pt_idx <= 7) used_types_mask |= static_cast<uint8_t>(1u << (pt_idx - 1));
    } else {
      ++t.defender_moves;
      // KING is encoded outside raw_type_of()'s three-bit unpromoted mask.
      if (type_of(moved) == KING) ++t.defender_king_moves;
      else ++t.non_king_defences;
    }
    bool sacrifice = false;
    if (i % 2 == 0 && i + 1 < line.size()) {
      // The following defence captures the checking piece on its arrival square.
      sacrifice = !is_drop(line[i + 1]) && from_sq(line[i + 1]) != to_sq(m) &&
                  to_sq(line[i + 1]) == to_sq(m);
      if (sacrifice) {
        ++t.sacrifices;
        const PieceType pt = raw_type_of(moved);
        if (pt == ROOK || pt == BISHOP) ++t.major_sacrifices;
        if (previous_sacrifice) ++t.consecutive_sacrifices;
      }
    }
    previous_sacrifice = sacrifice;
    pos.do_move(m, states[i]);
    if (i % 2 == 0) {
      const Square king = pos.king_square(pos.side_to_move());
      const Square arrival = to_sq(m);
      const int file_distance = std::abs(static_cast<int>(file_of(arrival)) - static_cast<int>(file_of(king)));
      const int rank_distance = std::abs(static_cast<int>(rank_of(arrival)) - static_cast<int>(rank_of(king)));
      if (std::max(file_distance, rank_distance) <= 1) ++t.contact_checks;
      if (pos.checkers().pop_count() > 1) ++t.double_checks;
      else if (!pos.checkers().test(arrival)) ++t.discovered_checks;
    }
  }
  // ループ後: posは詰み局面 — X4・X12・X9を論文通り詰み局面で計算
  if (!line.empty()) {
    // 詰み局面では pos.side_to_move() == 受け方（詰まれた側）
    const Color mated = pos.side_to_move();
    const Square ksq = pos.king_square(mated);
    const int kf = static_cast<int>(file_of(ksq));
    const int kr = static_cast<int>(rank_of(ksq));
    // X4: 詰み局面での王の開放度 (論文: 詰み局面で計算)
    for (int df = -1; df <= 1; ++df) {
      for (int dr = -1; dr <= 1; ++dr) {
        if (df == 0 && dr == 0) continue;
        const int nf = kf + df, nr = kr + dr;
        if (nf < 0 || nf > 8 || nr < 0 || nr > 8) continue;
        const Square adj = static_cast<Square>(nf * 9 + nr);
        const Piece p = pos.piece_on(adj);
        if (p == NO_PIECE || color_of(p) != mated) ++t.king_openness;
      }
    }
    // X12: 詰み局面での玉の辺からの距離 (中央ほど高得点)
    t.king_final_edge_dist = std::min({kf, 8 - kf, kr, 8 - kr});
  }
  // X9: 駒の使用率 = 使用した駒種数 / 7種
  t.piece_utilization = static_cast<double>(__builtin_popcount(used_types_mask)) / 7.0;

  for (auto it = line.rbegin(); it != line.rend(); ++it) pos.undo_move(*it);
  if (t.sacrifices) t.names.push_back("sacrifice");
  if (t.major_sacrifices) t.names.push_back("major-piece sacrifice");
  if (t.consecutive_sacrifices) t.names.push_back("consecutive sacrifices");
  if (t.discovered_checks) t.names.push_back("discovered check");
  if (t.double_checks) t.names.push_back("double check");
  if (t.non_king_defences) t.names.push_back("non-king defence");
  const double king_escape_ratio = t.defender_moves ?
      static_cast<double>(t.defender_king_moves) / t.defender_moves : 0.0;
  const double contact_ratio = t.attacker_moves ?
      static_cast<double>(t.contact_checks) / t.attacker_moves : 0.0;
  t.chase_ratio = (king_escape_ratio + contact_ratio) / 2.0;
  t.chasing_mate = t.attacker_moves >= 3 && t.sacrifices == 0 &&
                   t.discovered_checks == 0 && t.double_checks == 0 &&
                   t.non_king_defences == 0 &&
                   (t.chase_ratio >= 0.75 || king_escape_ratio >= 0.90);
  if (t.chasing_mate) t.names.push_back("chasing mate");
  return t;
}

inline bool HasSurplusAttackerHand(Position& pos, const std::vector<Move>& line) {
  const Color attacker = pos.side_to_move();
  const Hand initial_hand = pos.hand_of(attacker);
  if (initial_hand == 0) return false;  // no initial hand pieces, nothing to be surplus
  std::vector<StateInfo> states(line.size());
  for (std::size_t i = 0; i < line.size(); ++i) pos.do_move(line[i], states[i]);
  const Hand final_hand = pos.hand_of(attacker);
  for (auto it = line.rbegin(); it != line.rend(); ++it) pos.undo_move(*it);
  // Surplus = initial hand pieces still in hand at the end (never dropped)
  // Heuristic: if final_count >= initial_count for any piece type, that type is surplus.
  // Edge case (drop then recapture same type) is accepted as rare.
  static constexpr PieceType kPieceTypes[] = {PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK};
  for (auto pt : kPieceTypes) {
    const int init_cnt = hand_count(initial_hand, pt);
    if (init_cnt <= 0) continue;
    if (hand_count(final_hand, pt) >= init_cnt) return true;
  }
  return false;
}

inline AestheticScores Score(Position& pos, const VerifyResult& v, const TechniqueSummary& t) {
  AestheticScores s;
  s.legalityScore = v.complete && v.proof == Proof::kMate && !v.prohibited_move ? 100 : 0;
  s.uniquenessScore = v.unique ? 100 : (v.proof == Proof::kMate ? 20 : 0);
  s.lengthScore = std::min(100.0, std::max(0, v.mate_ply) * 5.0);

  // X5: 配置の広さ (bounding box of board pieces, negative — compact = better)
  // X6: 駒数 (board piece count, negative)
  // X7: 駒種重み付き駒数 (negative — valuable idle pieces are wasteful)
  // X8: 駒数に対する駒種比率 (negative)
  const int board_pieces = pos.pieces().pop_count();
  double x5_spread = 0.0;
  double x7_weighted = 0.0, x8_ratio = 0.0;
  {
    // Approximate piece values: P=1, L=3, N=3, S=5, B=8, R=9, G=5
    static constexpr int kW[] = {0, 1, 3, 3, 5, 8, 9, 5};
    int min_f = 8, max_f = 0, min_r = 8, max_r = 0;
    uint8_t type_mask = 0;
    int w_total = 0;
    for (int sq = 0; sq < SQ_NB; ++sq) {
      const Piece p = pos.piece_on(static_cast<Square>(sq));
      if (p == NO_PIECE) continue;
      const int f = static_cast<int>(file_of(static_cast<Square>(sq)));
      const int r = static_cast<int>(rank_of(static_cast<Square>(sq)));
      min_f = std::min(min_f, f); max_f = std::max(max_f, f);
      min_r = std::min(min_r, r); max_r = std::max(max_r, r);
      const int rpt = static_cast<int>(raw_type_of(p));
      if (rpt >= 1 && rpt <= 7) { w_total += kW[rpt]; type_mask |= static_cast<uint8_t>(1u << (rpt - 1)); }
    }
    if (board_pieces > 0) {
      x5_spread = static_cast<double>((max_f - min_f) * (max_r - min_r)) / 64.0 * 100.0;
      x7_weighted = static_cast<double>(w_total) / board_pieces * 10.0;  // avg piece value
      x8_ratio = static_cast<double>(__builtin_popcount(type_mask)) / board_pieces * 100.0;
    }
  }

  // X10: 探索局面数 (search difficulty, positive — harder to solve = better)
  const double x10_difficulty = v.nodes > 0 ?
      std::min(100.0, std::log2(static_cast<double>(v.nodes)) /
               std::log2(50'000'000.0) * 100.0) : 0.0;

  // Store raw X values for regression export
  s.x5_spread = x5_spread;
  s.x6_count  = board_pieces;
  s.x7_weighted = x7_weighted;
  s.x8_ratio  = x8_ratio;
  s.x10_difficulty = x10_difficulty;

  // X3: 攻め方の駒取りはマイナス評価（追い詰め的で非美的）
  const double capture_penalty = std::min(50.0, t.captures_by_attacker * 15.0);

  // complexityScore: 変化の多さ + X10（難解さ）- X3（駒取り）
  s.complexityScore = std::max(0.0, std::min(100.0,
      static_cast<double>(v.longest_lines.size()) * 10.0 +
      t.non_king_defences * 8.0 + t.discovered_checks * 12.0 +
      x10_difficulty * 0.4) - capture_penalty);

  s.sacrificeScore = std::min(100.0, t.sacrifices * 18.0 + t.major_sacrifices * 22.0 +
                                      t.consecutive_sacrifices * 12.0);

  // techniqueScore: X1 + X2 + X9 + X11
  s.techniqueScore = std::min(100.0, t.sacrifices * 10.0 + t.major_sacrifices * 18.0 +
                                      t.consecutive_sacrifices * 15.0 +
                                      t.discovered_checks * 18.0 + t.double_checks * 20.0 +
                                      t.non_king_defences * 4.0 +
                                      t.non_promotions * 12.0 +       // X2: 不成
                                      t.piece_utilization * 25.0);    // X9

  // economyScore: X6（駒数少）- X5（配置広い）- X7（重い駒）- X8（種類比率）
  s.economyScore = std::max(0.0, 100.0 - std::max(0, board_pieces - 3) * 5.0
                                        - x5_spread * 0.25   // X5 penalty
                                        - x7_weighted * 0.5  // X7 penalty
                                        - x8_ratio * 0.15);  // X8 penalty
  if (v.unnecessary_piece) s.economyScore = 0;

  // visualScore: X4（詰み局面の王の開放度）+ X12（玉の位置）
  s.visualScore = std::min(100.0, std::max(0.0,
      85.0 - std::max(0, board_pieces - 8) * 3.0 +
      t.king_openness * 5.0 +         // X4: 詰み局面での開放度（論文通り）
      t.king_final_edge_dist * 8.0));  // X12: 玉が中央寄りほど高評価
  s.originalityScore = 50;
  s.aestheticScore = (s.lengthScore + s.complexityScore + s.sacrificeScore +
                      s.techniqueScore + s.economyScore + s.visualScore) / 6.0;
  // 変同による大幅減点: 変同1件につき-20点（最大-60点）
  if (v.hendou_count > 0)
    s.aestheticScore = std::max(0.0, s.aestheticScore - std::min(60.0, v.hendou_count * 20.0));
  if (t.chasing_mate) s.aestheticScore = std::min(s.aestheticScore, 20.0);
  s.totalScore = s.legalityScore * .18 + s.uniquenessScore * .18 + s.lengthScore * .10 +
                 s.complexityScore * .08 + s.sacrificeScore * .10 + s.techniqueScore * .10 +
                 s.economyScore * .09 + s.visualScore * .05 + s.originalityScore * .04 +
                 s.aestheticScore * .08;
  if (!(v.complete && v.proof == Proof::kMate && v.unique) || v.piece_surplus ||
      v.unnecessary_piece || v.futile_interposition || v.prohibited_move)
    s.totalScore = std::min(s.totalScore, 49.0);
  if (t.chasing_mate) s.totalScore = std::min(s.totalScore, 25.0);
  return s;
}

inline WorkRecord MakeRecord(Position& pos, const std::string& normalized_sfen,
                             const VerifyOptions& options, VerifyResult verification,
                             const std::string& parent_id = "") {
  WorkRecord r;
  r.sfen = pos.sfen();
  r.normalized_sfen = detail::NormalizeSfenHandText(normalized_sfen.empty() ? r.sfen : normalized_sfen);
  r.id = detail::StableId(r.sfen + detail::UsiLine(verification.principal));
  r.canonical_id = detail::StableId(r.normalized_sfen);
  r.parent_id = parent_id; r.double_king = options.double_king; r.conditions = options;
  r.attacker_hand = detail::HandSfen(pos.hand_of(BLACK), false);
  r.defender_hand = detail::HandSfen(pos.hand_of(WHITE), true);
  verification.piece_surplus = verification.proof == Proof::kMate &&
                               HasSurplusAttackerHand(pos, verification.principal);
  r.techniques = AnalyzeTechniques(pos, verification.principal);
  r.scores = Score(pos, verification, r.techniques);
  r.verification = std::move(verification);
  r.perfect = r.verification.complete && r.verification.proof == Proof::kMate && r.verification.unique &&
              !r.verification.line_alternative && !r.verification.shorter_mate && !r.verification.piece_surplus &&
              !r.verification.unnecessary_piece && !r.verification.prohibited_move;
  if (!r.perfect) r.aesthetic_reasons.push_back("composition completeness gate failed");
  if (r.verification.mate_ply < 7)
    r.aesthetic_reasons.push_back("shorter than the formal seven-ply minimum");
  if (r.techniques.chasing_mate) r.aesthetic_reasons.push_back("mechanical chasing mate");
  const bool meaningful_technique = r.techniques.sacrifices > 0 ||
      r.techniques.major_sacrifices > 0 || r.techniques.consecutive_sacrifices > 0 ||
      r.techniques.discovered_checks > 0 || r.techniques.double_checks > 0 ||
      r.techniques.non_king_defences > 0;
  if (!meaningful_technique) r.aesthetic_reasons.push_back("no detected thematic technique");
  if (r.techniques.non_king_defences == 0)
    r.aesthetic_reasons.push_back("all defender replies are king moves (lure-and-chase pattern)");
  if (r.verification.hendou_count > 0) {
    std::ostringstream hs;
    hs << "hendou (same-move-count alternatives): " << r.verification.hendou_count << " occurrences";
    r.aesthetic_reasons.push_back(hs.str());
  }
  if (r.scores.aestheticScore < 35.0) r.aesthetic_reasons.push_back("aesthetic score below 35");
  r.aesthetic_pass = r.perfect && r.verification.mate_ply >= 7 && !r.techniques.chasing_mate &&
                     meaningful_technique && r.techniques.non_king_defences > 0 &&
                     r.scores.aestheticScore >= 35.0;
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  std::ostringstream date; date << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ"); r.generated_at = date.str();
  return r;
}

inline std::string ToJson(const VerifyResult& r, Position& root) {
  std::ostringstream o;
  o << "{";
  o << "\"schemaVersion\":1,\"mode\":\"tsume\",\"sfen\":\"" << detail::EscapeJson(root.sfen()) << "\",";
  o << "\"status\":\"" << detail::ProofName(r.proof) << "\",\"complete\":" << (r.complete ? "true" : "false") << ',';
  o << "\"matePly\":" << r.mate_ply << ",\"nodes\":" << r.nodes << ',';
  o << "\"unique\":" << (r.unique ? "true" : "false") << ",\"alternativeMate\":" << (r.alternative_mate ? "true" : "false") << ',';
  o << "\"earlyMate\":" << (r.shorter_mate ? "true" : "false") << ",\"repetition\":" << (r.repetition ? "true" : "false") << ',';
  o << "\"usi\":\"" << detail::EscapeJson(detail::UsiLine(r.principal)) << "\",";
  o << "\"human\":\"" << detail::EscapeJson(detail::HumanLine(root, r.principal)) << "\",";
  o << "\"attacks\":[";
  for (std::size_t i = 0; i < r.attacks.size(); ++i) { const auto& b = r.attacks[i]; if (i) o << ','; o << "{\"move\":\"" << USI::move(b.move) << "\",\"status\":\"" << detail::ProofName(b.proof) << "\",\"matePly\":" << b.mate_ply << ",\"line\":\"" << detail::EscapeJson(detail::UsiLine(b.principal)) << "\"}"; }
  o << "],\"hendouCount\":" << r.hendou_count << ",\"sameLengthVariations\":[";
  for (std::size_t i = 0; i < r.longest_lines.size(); ++i) { if (i) o << ','; o << '"' << detail::EscapeJson(detail::UsiLine(r.longest_lines[i].moves)) << '"'; }
  o << "],\"reasons\":[";
  for (std::size_t i = 0; i < r.reasons.size(); ++i) { if (i) o << ','; o << '"' << detail::EscapeJson(r.reasons[i]) << '"'; }
  o << "]}";
  return o.str();
}

inline void WriteScores(std::ostringstream& o, const AestheticScores& s) {
  o << "\"legalityScore\":" << s.legalityScore << ",\"uniquenessScore\":" << s.uniquenessScore
    << ",\"lengthScore\":" << s.lengthScore << ",\"complexityScore\":" << s.complexityScore
    << ",\"sacrificeScore\":" << s.sacrificeScore << ",\"techniqueScore\":" << s.techniqueScore
    << ",\"economyScore\":" << s.economyScore << ",\"visualScore\":" << s.visualScore
    << ",\"originalityScore\":" << s.originalityScore << ",\"aestheticScore\":" << s.aestheticScore
    << ",\"totalScore\":" << s.totalScore
    << ",\"x5Spread\":" << s.x5_spread << ",\"x6Count\":" << s.x6_count
    << ",\"x7Weighted\":" << s.x7_weighted << ",\"x8Ratio\":" << s.x8_ratio
    << ",\"x10Difficulty\":" << s.x10_difficulty;
}

inline std::string ToJson(const WorkRecord& r, Position& root) {
  std::ostringstream o;
  o << "{\"schemaVersion\":1,\"id\":\"" << r.id << "\",\"canonicalId\":\"" << r.canonical_id
    << "\",\"parentId\":\"" << detail::EscapeJson(r.parent_id) << "\",\"sfen\":\""
    << detail::EscapeJson(r.sfen) << "\",\"normalizedSfen\":\"" << detail::EscapeJson(r.normalized_sfen)
    << "\",\"kingMode\":\"" << (r.double_king ? "double" : "single") << "\",\"matePly\":"
    << r.verification.mate_ply << ",\"attackerHand\":\"" << r.attacker_hand
    << "\",\"defenderHand\":\"" << r.defender_hand << "\",\"usi\":\""
    << detail::EscapeJson(detail::UsiLine(r.verification.principal))
    << "\",\"human\":\"" << detail::EscapeJson(detail::HumanLine(root, r.verification.principal)) << "\",";
  o << "\"complete\":" << (r.verification.complete ? "true" : "false")
    << ",\"status\":\"" << detail::ProofName(r.verification.proof) << "\""
    << ",\"perfect\":" << (r.perfect ? "true" : "false")
    << ",\"unique\":" << (r.verification.unique ? "true" : "false")
    << ",\"alternativeMate\":" << (r.verification.alternative_mate ? "true" : "false")
    << ",\"lineAlternativeMate\":" << (r.verification.line_alternative ? "true" : "false")
    << ",\"earlyMate\":" << (r.verification.shorter_mate ? "true" : "false")
    << ",\"noMateBranch\":" << (r.verification.no_mate_branch ? "true" : "false")
    << ",\"pieceSurplus\":" << (r.verification.piece_surplus ? "true" : "false")
    << ",\"unnecessaryPiece\":" << (r.verification.unnecessary_piece ? "true" : "false")
    << ",\"futileInterposition\":" << (r.verification.futile_interposition ? "true" : "false")
    << ",\"repetition\":" << (r.verification.repetition ? "true" : "false")
    << ",\"prohibitedMove\":" << (r.verification.prohibited_move ? "true" : "false")
    << ",\"intendedMismatch\":" << (r.verification.intended_mismatch ? "true" : "false")
    << ",\"symmetricSolutionsCollapsed\":" << (r.verification.symmetric_solutions_collapsed ? "true" : "false")
    << ",\"nodes\":" << r.verification.nodes << ",\"maxPly\":" << r.conditions.max_ply
    << ",\"maxNodes\":" << r.conditions.max_nodes << ",\"generatedAt\":\"" << r.generated_at << "\",";
  o << "\"scores\":{"; WriteScores(o, r.scores); o << "},\"techniques\":[";
  for (std::size_t i = 0; i < r.techniques.names.size(); ++i) { if (i) o << ','; o << '"' << detail::EscapeJson(r.techniques.names[i]) << '"'; }
  o << "],\"aestheticPass\":" << (r.aesthetic_pass ? "true" : "false")
    << ",\"aestheticReasons\":[";
  for (std::size_t i = 0; i < r.aesthetic_reasons.size(); ++i) {
    if (i) o << ',';
    o << '"' << detail::EscapeJson(r.aesthetic_reasons[i]) << '"';
  }
  o << "],\"chaseRatio\":" << r.techniques.chase_ratio
    << ",\"aestheticMetrics\":{\"attackerMoves\":" << r.techniques.attacker_moves
    << ",\"contactChecks\":" << r.techniques.contact_checks
    << ",\"defenderMoves\":" << r.techniques.defender_moves
    << ",\"defenderKingMoves\":" << r.techniques.defender_king_moves
    << ",\"nonKingDefences\":" << r.techniques.non_king_defences
    << ",\"sacrifices\":" << r.techniques.sacrifices
    << ",\"discoveredChecks\":" << r.techniques.discovered_checks
    << ",\"doubleChecks\":" << r.techniques.double_checks
    << ",\"capturesByAttacker\":" << r.techniques.captures_by_attacker
    << ",\"nonPromotions\":" << r.techniques.non_promotions
    << ",\"kingOpenness\":" << r.techniques.king_openness
    << ",\"pieceUtilization\":" << r.techniques.piece_utilization
    << ",\"kingFinalEdgeDist\":" << r.techniques.king_final_edge_dist << "}"
    << ",\"attacks\":[";
  for (std::size_t i = 0; i < r.verification.attacks.size(); ++i) {
    const auto& b = r.verification.attacks[i]; if (i) o << ',';
    o << "{\"move\":\"" << USI::move(b.move) << "\",\"status\":\"" << detail::ProofName(b.proof)
      << "\",\"matePly\":" << b.mate_ply << ",\"counterexample\":\""
      << detail::EscapeJson(detail::UsiLine(b.principal)) << "\"}";
  }
  o << "],\"lineAlternatives\":[";
  for (std::size_t i = 0; i < r.verification.line_alternatives.size(); ++i) {
    const auto& [ply, b] = r.verification.line_alternatives[i]; if (i) o << ',';
    o << "{\"ply\":" << ply + 1 << ",\"move\":\"" << USI::move(b.move) << "\",\"matePly\":" << b.mate_ply
      << ",\"line\":\"" << detail::EscapeJson(detail::UsiLine(b.principal)) << "\"}";
  }
  o << "],\"variations\":[";
  for (std::size_t i = 0; i < r.verification.longest_lines.size(); ++i) { if (i) o << ','; o << '"' << detail::EscapeJson(detail::UsiLine(r.verification.longest_lines[i].moves)) << '"'; }
  o << "],\"unnecessarySquares\":[";
  for (std::size_t i = 0; i < r.verification.unnecessary_squares.size(); ++i) { if (i) o << ','; o << '"' << r.verification.unnecessary_squares[i] << '"'; }
  o << "],\"reasons\":[";
  for (std::size_t i = 0; i < r.verification.reasons.size(); ++i) { if (i) o << ','; o << '"' << detail::EscapeJson(r.verification.reasons[i]) << '"'; }
  o << "],\"title\":\"" << detail::EscapeJson(r.title) << "\",\"author\":\"" << detail::EscapeJson(r.author)
    << "\",\"notes\":\"" << detail::EscapeJson(r.notes) << "\",\"parentDiff\":\""
    << detail::EscapeJson(r.parent_diff) << "\",\"parentSimilarity\":" << r.parent_similarity
    << ",\"parentMatePly\":" << r.parent_mate_ply << ",\"extensionReason\":\""
    << detail::EscapeJson(r.extension_reason) << "\"}";
  return o.str();
}

inline bool SaveJson(const WorkRecord& record, Position& root, const std::string& filename) {
  std::ofstream out(filename, std::ios::binary | std::ios::app);
  if (!out) return false;
  out << ToJson(record, root) << '\n';
  return out.good();
}

inline std::string ExtractJsonString(const std::string& json, const std::string& key) {
  const std::string marker = "\"" + key + "\":\"";
  auto p = json.find(marker); if (p == std::string::npos) return {};
  p += marker.size(); std::string out; bool escaped = false;
  for (; p < json.size(); ++p) {
    const char c = json[p];
    if (escaped) { if (c == 'n') out += '\n'; else if (c == 'r') out += '\r'; else if (c == 't') out += '\t'; else out += c; escaped = false; }
    else if (c == '\\') escaped = true;
    else if (c == '"') break;
    else out += c;
  }
  return out;
}

inline bool LoadSfenFromJson(const std::string& filename, std::string& sfen) {
  std::ifstream in(filename, std::ios::binary); if (!in) return false;
  std::string line, last; while (std::getline(in, line)) if (!line.empty()) last = line;
  sfen = ExtractJsonString(last, "sfen"); return !sfen.empty();
}

}  // namespace komori::tsume

#endif  // KOMORI_TSUME_WORKBENCH_HPP_
