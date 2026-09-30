#include <random>
#include <unordered_set>

#include "komoring_heights.hpp"
#include "path_keys.hpp"
#include "thread_initialization.hpp"
#include "tsume_generator.hpp"
#include "tsume_validator.hpp"
#include "tsume_workbench.hpp"
#include "typedefs.hpp"

#if defined(USER_ENGINE)

namespace {
/// 探索器。[0] は通常用、[1] は tsume_answer の合駒割引（下界）探索用で、置換表を分けて持つ。
komori::KomoringHeights g_searchers[2];
komori::KomoringHeights* g_active_searcher = &g_searchers[0];
komori::KomoringHeights& Searcher() { return *g_active_searcher; }
komori::EngineOption g_option;
std::atomic_bool g_path_key_init_flag;

komori::NodeState g_search_result = komori::NodeState::kUnknown;

/// tsume_generate / tsume_batch_generate 実行中に checkmate/bestmove 出力を抑制するフラグ
bool g_tsume_gen_silent = false;

/// 局面が OR node っぽいかどうかを調べる。困ったら OR node として処理する。
bool IsPosOrNode(const Position& root_pos) {
  const Color us = root_pos.side_to_move();
  const Color them = ~us;

  if (root_pos.king_square(us) == SQ_NB) {
    return true;
  } else if (root_pos.king_square(them) == SQ_NB) {
    return false;
  }

  if (root_pos.in_check() && g_option.root_is_and_node_if_checked) {
    return false;
  }
  return true;
}

enum class LoseKind {
  kTimeout,
  kNoMate,
  kMate,
};

void PrintResult(bool is_mate_search, LoseKind kind, const std::string& pv_moves = "resign") {
  if (g_tsume_gen_silent) return;

  if (is_mate_search) {
    switch (kind) {
      case LoseKind::kTimeout:
        sync_cout << "checkmate timeout" << sync_endl;
        break;
      case LoseKind::kNoMate:
        sync_cout << "checkmate nomate" << sync_endl;
        break;
      default:
        sync_cout << "checkmate " << pv_moves << sync_endl;
    }
  }
  // `go` モードの場合は KomoringHeights::Search() 内で出力済み
}

// -----------------------------------------------------------------------
// 内部ヘルパー: 指定手数の詰将棋を count 問生成して found へ追加する
//
// 詰将棋ルール準拠:
//   - 攻め方は毎手王手のみ（OR ノードで王手のみ生成）
//   - 打ち歩詰め禁止（合法手生成で除外済み）
//   - 連続王手の千日手は攻め方の負け（repetition 検出）
//   - 受け方は最善手で応じる（AND ノードで全回避手評価）
// -----------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 余剰持ち駒除去: 作意で使われなかった攻め方の持ち駒を SFEN から取り除き、
// 受け方の予備に戻す。完全性ゲートを通過させるための前処理。
// ---------------------------------------------------------------------------
static std::string StripSurplusAttackerHand(
    const std::string& sfen, Position& pos, const std::vector<Move>& solution) {
  const Color attacker = pos.side_to_move();
  std::vector<StateInfo> states(solution.size());
  for (std::size_t i = 0; i < solution.size(); ++i)
    pos.do_move(solution[i], states[i]);
  const Hand surplus = pos.hand_of(attacker);
  for (auto it = solution.rbegin(); it != solution.rend(); ++it)
    pos.undo_move(*it);
  if (surplus == 0) return sfen;  // no surplus

  // Piece types with surplus counts
  static constexpr PieceType kHandTypes[] = {PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK};
  static constexpr char      kHandChars[] = {'P',  'L',   'N',    'S',    'G',  'B',    'R'};
  int surplus_count[7]{};
  for (int i = 0; i < 7; ++i)
    surplus_count[i] = hand_count(surplus, kHandTypes[i]);

  // Rebuild hand string with surplus attacker pieces removed
  std::istringstream ss(sfen);
  std::string board_str, turn, hand_str, move_num;
  ss >> board_str >> turn >> hand_str >> move_num;

  std::string new_hand;
  if (hand_str != "-") {
    int cnt = 0;
    for (char c : hand_str) {
      if (std::isdigit(static_cast<unsigned char>(c))) {
        cnt = cnt * 10 + (c - '0'); continue;
      }
      int n = cnt ? cnt : 1; cnt = 0;
      if (std::isupper(static_cast<unsigned char>(c))) {
        // Attacker's piece: subtract surplus
        int keep = n;
        for (int i = 0; i < 7; ++i)
          if (kHandChars[i] == c) { keep = std::max(0, n - surplus_count[i]); break; }
        if (keep > 1) new_hand += std::to_string(keep);
        if (keep > 0) new_hand += c;
      } else {
        // Defender's piece: keep as-is
        if (n > 1) new_hand += std::to_string(n);
        new_hand += c;
      }
    }
  }
  if (new_hand.empty()) new_hand = "-";
  // 剥ぎ取った攻め方の駒を受け方持ち駒に加える（CompleteDefenderReserve は
  // 全余剰駒を一括追加するため使わない。必要分だけ追記する）
  for (int i = 0; i < 7; ++i) {
    const int add = surplus_count[i];
    if (add <= 0) continue;
    const char lc = static_cast<char>(std::tolower(
        static_cast<unsigned char>(kHandChars[i])));
    if (new_hand == "-") new_hand.clear();
    if (add > 1) new_hand += std::to_string(add);
    new_hand += lc;
  }
  if (new_hand.empty()) new_hand = "-";
  return board_str + " " + turn + " " + new_hand + " " + move_num;
}
// ---------------------------------------------------------------------------
// 邪魔駒確認: 盤上の各駒（玉以外）を除いても同じ手数で詰むなら除去する
// ---------------------------------------------------------------------------
static komori::TsumeGeneratedProblem RemoveUnnecessaryPieces(
    const komori::TsumeGeneratedProblem& problem, std::uint64_t time_limit_ms) {
  std::istringstream ss(problem.sfen);
  std::string board_str, turn, hand_str, move_num;
  ss >> board_str >> turn >> hand_str >> move_num;

  komori::detail::SfenBoard board;
  komori::detail::ParseSfenBoard(board_str, board);

  bool changed = true;
  std::vector<std::string> current_sol = problem.solution;

  while (changed) {
    changed = false;
    for (int r = 0; r < 9 && !changed; ++r) {
      for (int f = 0; f < 9 && !changed; ++f) {
        const std::string& piece = board[r][f];
        if (piece.empty() || piece == "k" || piece == "K") continue;

        auto test_board = board;
        test_board[r][f] = "";
        // 除去した駒を受け方持ち駒に補充して39枚制を維持
        const std::string raw_test =
            komori::detail::BuildSfenBoard(test_board) + " " + turn + " " + hand_str + " " + move_num;
        const std::string test_sfen = komori::detail::CompleteDefenderReserve(raw_test, false);

        Position tp; StateListPtr st(new StateList(1));
        tp.set(test_sfen, &st->back(), Threads.main());
        if (!komori::ValidateTsumePosition(tp).empty() || tp.in_check()) continue;

        // 邪魔駒チェックは短時間で十分（邪魔駒は即座に詰み確認できる）
        const std::uint64_t removal_limit_ms = static_cast<std::uint64_t>(100);
        Search::LimitsType limits; limits.mate = static_cast<int>(removal_limit_ms);
        Time.reset();
        Threads.start_thinking(tp, st, limits, false);
        Threads.main()->wait_for_search_finished();

        if (g_search_result == komori::NodeState::kProven) {
          const auto& bm = Searcher().BestMoves();
          if (static_cast<int>(bm.size()) == problem.mate_in) {
            board[r][f] = "";
            board_str = komori::detail::BuildSfenBoard(board);
            current_sol.clear();
            for (const auto& m : bm) current_sol.push_back(USI::move(m));
            changed = true;
            sync_cout << "info string [邪魔駒] 除去: " << piece
                      << " (" << (9 - f) << static_cast<char>('a' + r) << ")" << sync_endl;
          }
        }
      }
    }
  }

  const std::string result_raw = komori::detail::BuildSfenBoard(board) + " " + turn + " " + hand_str + " " + move_num;
  return {komori::detail::CompleteDefenderReserve(result_raw, false),
          problem.mate_in, current_sol, ""};
}

// ---------------------------------------------------------------------------
// 手数伸ばし: 攻め方の盤上駒を別マスに動かして再探索、長くなれば採用
// ---------------------------------------------------------------------------
static komori::TsumeGeneratedProblem TryExtendMateLen(
    const komori::TsumeGeneratedProblem& problem, int max_target,
    std::mt19937& rng, std::uint64_t time_limit_ms) {
  std::istringstream ss(problem.sfen);
  std::string board_str, turn, hand_str, move_num;
  ss >> board_str >> turn >> hand_str >> move_num;

  komori::detail::SfenBoard board;
  komori::detail::ParseSfenBoard(board_str, board);

  // 攻め方の盤上駒（大文字・玉以外）を収集
  std::vector<std::pair<int,int>> atk;
  for (int r = 0; r < 9; ++r)
    for (int f = 0; f < 9; ++f) {
      const auto& p = board[r][f];
      if (!p.empty() && std::isupper(static_cast<unsigned char>(p.back())) && p != "K")
        atk.push_back({r, f});
    }
  if (atk.empty()) return problem;

  komori::TsumeGeneratedProblem best = problem;
  std::uniform_int_distribution<int> rd(0, 8), fd(0, 8);
  std::uniform_int_distribution<int> pd(0, static_cast<int>(atk.size()) - 1);

  for (int attempt = 0; attempt < 200; ++attempt) {
    auto [pr, pf] = atk[pd(rng)];
    const std::string piece = board[pr][pf];

    int nr = -1, nf = -1;
    for (int t = 0; t < 30; ++t) {
      int tr = rd(rng), tf = fd(rng);
      if (!board[tr][tf].empty()) continue;
      if (!komori::detail::IsValidBoardPlacement(piece.back(), tr)) continue;
      nr = tr; nf = tf; break;
    }
    if (nr < 0) continue;

    auto test_board = board;
    test_board[pr][pf] = "";
    test_board[nr][nf] = piece;
    const std::string test_sfen =
        komori::detail::BuildSfenBoard(test_board) + " " + turn + " " + hand_str + " " + move_num;

    Position tp; StateListPtr st(new StateList(1));
    tp.set(test_sfen, &st->back(), Threads.main());
    if (!komori::ValidateTsumePosition(tp).empty() || tp.in_check()) continue;

    Search::LimitsType limits; limits.mate = static_cast<int>(time_limit_ms * 2);
    Time.reset();
    Threads.start_thinking(tp, st, limits, false);
    Threads.main()->wait_for_search_finished();

    if (g_search_result == komori::NodeState::kProven) {
      const auto& bm = Searcher().BestMoves();
      const int new_mate = static_cast<int>(bm.size());
      if (new_mate > best.mate_in && new_mate <= max_target) {
        std::vector<std::string> sol;
        for (const auto& m : bm) sol.push_back(USI::move(m));
        best = {test_sfen, new_mate, sol, ""};
        sync_cout << "info string [手数伸ばし] " << problem.mate_in << "→" << new_mate
                  << "手: " << test_sfen << sync_endl;
      }
    }
  }
  return best;
}

static std::string VerificationSignature(const komori::tsume::VerifyResult& r) {
  std::vector<std::string> branches, lines;
  for (const auto& b : r.attacks)
    branches.push_back(USI::move(b.move) + ":" + komori::tsume::detail::ProofName(b.proof) +
                       ":" + std::to_string(b.mate_ply));
  for (const auto& l : r.longest_lines) lines.push_back(komori::tsume::detail::UsiLine(l.moves));
  std::sort(branches.begin(), branches.end()); std::sort(lines.begin(), lines.end());
  std::ostringstream out;
  out << komori::tsume::detail::ProofName(r.proof) << ':' << r.mate_ply << ':' << r.unique << ':';
  for (const auto& x : branches) out << x << ';'; out << '|';
  for (const auto& x : lines) out << x << ';';
  return out.str();
}

static std::string IsomorphismSignature(const komori::tsume::VerifyResult& r) {
  std::vector<std::string> branches;
  for (const auto& b : r.attacks)
    branches.push_back(std::string(komori::tsume::detail::ProofName(b.proof)) + ":" +
                       std::to_string(b.mate_ply) + ":" + std::to_string(b.principal.size()));
  std::sort(branches.begin(), branches.end());
  std::vector<std::size_t> variation_lengths;
  for (const auto& l : r.longest_lines) variation_lengths.push_back(l.moves.size());
  std::sort(variation_lengths.begin(), variation_lengths.end());
  std::ostringstream out; out << komori::tsume::detail::ProofName(r.proof) << ':' << r.mate_ply
                              << ':' << r.unique << ':' << r.complete << '|';
  for (const auto& x : branches) out << x << ';'; out << '|';
  for (auto x : variation_lengths) out << x << ';';
  return out.str();
}

static bool VerifyIsomorphic(const std::string& sfen, const komori::tsume::VerifyOptions& options,
                             const std::string& wanted, komori::tsume::VerifyResult* result = nullptr) {
  Position p; StateListPtr states(new StateList(1)); p.set(sfen, &states->back(), Threads.main());
  if (!komori::ValidateTsumePosition(p).empty()) return false;
  komori::tsume::ExhaustiveVerifier verifier(options); auto checked = verifier.Run(p);
  if (!checked.complete || IsomorphismSignature(checked) != wanted) return false;
  if (result) *result = std::move(checked);
  return true;
}

// Normalize only through re-proven isomorphisms: maximize upward movement,
// then rightward movement, then consider a mirror under the documented tie-break.
static std::string CanonicalizeVerified(const std::string& sfen,
                                        const komori::tsume::VerifyOptions& options,
                                        const komori::tsume::VerifyResult& original) {
  std::istringstream in(sfen); std::string board_text, turn, hands, move_no;
  in >> board_text >> turn >> hands >> move_no;
  const std::string wanted = IsomorphismSignature(original);
  auto normalize = [&](komori::detail::SfenBoard board) {
    for (;;) {
      bool can = true; for (int f = 0; f < 9; ++f) if (!board[0][f].empty()) can = false;
      if (!can) break;
      auto next = komori::detail::TransformBoard(board, false, 0, 1);
      const std::string candidate = komori::detail::BuildSfenBoard(next) + " " + turn + " " + hands + " " + move_no;
      if (!VerifyIsomorphic(candidate, options, wanted)) break; board = std::move(next);
    }
    for (;;) {
      bool can = true; for (int r = 0; r < 9; ++r) if (!board[r][8].empty()) can = false;
      if (!can) break;
      auto next = komori::detail::TransformBoard(board, false, -1, 0);
      const std::string candidate = komori::detail::BuildSfenBoard(next) + " " + turn + " " + hands + " " + move_no;
      if (!VerifyIsomorphic(candidate, options, wanted)) break; board = std::move(next);
    }
    return board;
  };
  komori::detail::SfenBoard source; komori::detail::ParseSfenBoard(board_text, source);
  auto normal = normalize(source);
  auto mirrored = normalize(komori::detail::TransformBoard(source, true, 0, 0));
  const std::string ns = komori::detail::BuildSfenBoard(normal) + " " + turn + " " + hands + " " + move_no;
  const std::string ms = komori::detail::BuildSfenBoard(mirrored) + " " + turn + " " + hands + " " + move_no;
  const bool mirror_ok = VerifyIsomorphic(ms, options, wanted);
  if (!mirror_ok) return ns;
  auto priority = [](const komori::detail::SfenBoard& b) {
    int king = -1, major = 0;
    for (int r = 0; r < 9; ++r) for (int f = 0; f < 9; ++f) {
      if (b[r][f] == "k") king = f;
      if (b[r][f] == "R" || b[r][f] == "+R" || b[r][f] == "B" || b[r][f] == "+B") major += f;
    }
    return std::pair<int,int>{king, major};
  };
  const auto np = priority(normal), mp = priority(mirrored);
  if (mp > np) return ms; if (np > mp) return ns;
  return std::min(ns, ms);
}

// A piece is unnecessary only if removing it (and consequently restoring it
// to the defender reserve) preserves the complete bounded solution tree.
static void AnalyzeUnnecessaryPieces(const std::string& completed_sfen,
                                     const komori::tsume::VerifyOptions& options,
                                     komori::tsume::VerifyResult& original) {
  if (!original.complete) return;
  std::istringstream in(completed_sfen);
  std::string board_text, turn, hands, move_no; in >> board_text >> turn >> hands >> move_no;
  komori::detail::SfenBoard board; komori::detail::ParseSfenBoard(board_text, board);
  const std::string signature = VerificationSignature(original);
  for (int r = 0; r < 9; ++r) for (int f = 0; f < 9; ++f) {
    if (board[r][f].empty() || board[r][f] == "K" || board[r][f] == "k") continue;
    auto reduced = board; reduced[r][f].clear();
    std::string test_sfen = komori::detail::BuildSfenBoard(reduced) + " " + turn + " " + hands + " " + move_no;
    test_sfen = komori::detail::CompleteDefenderReserve(test_sfen, options.double_king);
    Position test; StateListPtr states(new StateList(1)); test.set(test_sfen, &states->back(), Threads.main());
    if (!komori::ValidateTsumePosition(test).empty()) continue;
    komori::tsume::ExhaustiveVerifier verifier(options); auto result = verifier.Run(test);
    original.nodes += result.nodes;
    if (result.complete && VerificationSignature(result) == signature) {
      original.unnecessary_piece = true;
      original.unnecessary_squares.push_back(std::to_string(9 - f) + char('a' + r));
    }
  }
}

// 審美惜しく不合格の局面（EV合格・変同1-3）を PerturbAndRetry 用にキャプチャする
// TryCandidate が審美却下時に書き込む。GenerateProblemsForMoves が読み出して活用する。
struct AestheticNearMiss {
  std::string sfen;
  int hendou_count = -1;
  int target_moves = 0;
  std::vector<std::string> solution;
  bool valid() const { return hendou_count >= 1 && hendou_count <= 3 && !sfen.empty(); }
  void clear() { sfen.clear(); hendou_count = -1; }
};
static AestheticNearMiss g_aesthetic_near_miss;

// 試す1個のSFEN候補をdf-pn+EV+aesthetic gateに通す共通ヘルパー
// 合格したら TsumeGeneratedProblem を返す。不合格や検証失敗なら nullopt。
static std::optional<komori::TsumeGeneratedProblem> TryCandidate(
    const std::string& new_sfen,
    const komori::tsume::VerifyOptions& verify_options,
    int target_moves,
    std::uint64_t time_limit_ms) {
  Position candidate; StateListPtr states(new StateList(1));
  candidate.set(new_sfen, &states->back(), Threads.main());
  if (!komori::ValidateTsumePosition(candidate).empty()) return std::nullopt;
  if (candidate.in_check()) return std::nullopt;

  Search::LimitsType limits;
  limits.mate = static_cast<int>(time_limit_ms / 3);
  Time.reset();
  Threads.start_thinking(candidate, states, limits, false);
  Threads.main()->wait_for_search_finished();

  if (g_search_result != komori::NodeState::kProven) {
    sync_cout << "info string [TC reject] dfpn-fail " << new_sfen << sync_endl;
    return std::nullopt;
  }
  const auto& bm = Searcher().BestMoves();
  if (static_cast<int>(bm.size()) != target_moves) {
    sync_cout << "info string [TC reject] ply=" << bm.size() << " want=" << target_moves
              << " " << new_sfen << sync_endl;
    return std::nullopt;
  }
  if (komori::tsume::HasSurplusAttackerHand(candidate, bm)) {
    sync_cout << "info string [TC reject] surplus-hand " << new_sfen << sync_endl;
    return std::nullopt;
  }

  {
    komori::tsume::VerifyOptions quick_opts;
    quick_opts.max_ply = target_moves;
    quick_opts.max_nodes = std::max<std::uint64_t>(1'500'000ULL,
        komori::detail::ComputeNodesLimit(target_moves));
    quick_opts.double_king = false;
    Position qp; StateListPtr qs(new StateList(1));
    qp.set(new_sfen, &qs->back(), Threads.main());
    komori::tsume::ExhaustiveVerifier qev(quick_opts);
    const auto qr = qev.Run(qp);
    if (qr.complete && (!qr.unique || qr.proof != komori::tsume::Proof::kMate ||
                        qr.mate_ply != target_moves)) {
      sync_cout << "info string [TC reject] quick-ev unique=" << qr.unique
                << " ply=" << qr.mate_ply << " " << new_sfen << sync_endl;
      return std::nullopt;
    }
  }

  Position verify_pos; StateListPtr vs(new StateList(1));
  verify_pos.set(new_sfen, &vs->back(), Threads.main());
  komori::tsume::ExhaustiveVerifier ev(verify_options);
  auto result = ev.Run(verify_pos);
  // complete=false かつ ply==target_moves の場合のみ再試行（パス発見済みだが完了できず）
  // ply=-1（パス未発見）は再試行しない: 250Mノードでも詰まる可能性があり非常に高コスト
  if (!result.complete && result.mate_ply == target_moves) {
    sync_cout << "info string [EV再試行] complete=0 ply=" << target_moves
              << " → 5倍ノードで再試行: " << new_sfen << sync_endl;
    komori::tsume::VerifyOptions retry_opts = verify_options;
    retry_opts.max_nodes = verify_options.max_nodes * 5;
    Position retry_pos2; StateListPtr rs2(new StateList(1));
    retry_pos2.set(new_sfen, &rs2->back(), Threads.main());
    komori::tsume::ExhaustiveVerifier ev2(retry_opts);
    const auto result2 = ev2.Run(retry_pos2);
    if (result2.complete) {
      // 再試行で完了: verify_pos を再初期化して result2 に差し替え
      verify_pos.set(new_sfen, &vs->back(), Threads.main());
      result = result2;
    }
    // complete=false のままなら result は初回のまま（その後の complete チェックで弾かれる）
  }
  // EV完了時のみ余剰駒チェック: EV未完了時は principal が不完全で誤strip を引き起こす
  if (result.complete && komori::tsume::HasSurplusAttackerHand(verify_pos, result.principal)) {
    // surplus-ev: 攻め方手持ちに使われない駒がある → stripped 局面で再試行
    const std::string stripped = komori::detail::StripSurplusAttackerHand(
        new_sfen, verify_pos, result.principal);
    if (!stripped.empty() && stripped != new_sfen) {
      sync_cout << "info string [surplus-ev strip] " << stripped << sync_endl;
      return TryCandidate(stripped, verify_options, target_moves, time_limit_ms);
    }
    sync_cout << "info string [TC reject] surplus-ev " << new_sfen << sync_endl;
    return std::nullopt;
  }
  if (!result.complete || result.proof != komori::tsume::Proof::kMate ||
      !result.unique || result.mate_ply != target_moves) {
    sync_cout << "info string [TC reject] ev fail complete=" << result.complete
              << " unique=" << result.unique << " ply=" << result.mate_ply << sync_endl;
    return std::nullopt;
  }

  AnalyzeUnnecessaryPieces(new_sfen, verify_options, result);
  if (result.unnecessary_piece) return std::nullopt;

  auto record = komori::tsume::MakeRecord(verify_pos, new_sfen, verify_options,
                                          std::move(result));
  if (!record.aesthetic_pass) {
    sync_cout << "info string [TryCandidate] 審美却下 hendou=" << record.verification.hendou_count
              << " " << new_sfen << sync_endl;
    // 変同が少なく EV 合格の局面は近傍摂動の種として保存
    if (record.verification.complete && record.verification.unique &&
        record.verification.proof == komori::tsume::Proof::kMate &&
        record.verification.mate_ply == target_moves &&
        record.verification.hendou_count >= 1 && record.verification.hendou_count <= 3) {
      g_aesthetic_near_miss.sfen = new_sfen;
      g_aesthetic_near_miss.hendou_count = record.verification.hendou_count;
      g_aesthetic_near_miss.target_moves = target_moves;
      g_aesthetic_near_miss.solution.clear();
      for (Move m : record.verification.principal)
        g_aesthetic_near_miss.solution.push_back(USI::move(m));
    }
    return std::nullopt;
  }

  komori::TsumeGeneratedProblem prob;
  prob.sfen = new_sfen;
  prob.mate_in = target_moves;
  for (Move m : record.verification.principal) prob.solution.push_back(USI::move(m));
  prob.record_json = komori::tsume::ToJson(record, verify_pos);
  return prob;
}

// 2駒同時移動による類似形探索: board上の非王駒2個をそれぞれ全マスに移動して試す
static std::optional<komori::TsumeGeneratedProblem> PerturbTwoPiece(
    const komori::TsumeGeneratedProblem& base_problem,
    const komori::tsume::VerifyOptions& verify_options,
    int target_moves,
    std::uint64_t time_limit_ms,
    std::uint64_t budget_ms = 120'000) {  // 最大2分で打ち切り
  const auto t0 = std::chrono::steady_clock::now();
  std::istringstream ss(base_problem.sfen);
  std::string board_str, turn, hand_str, move_no;
  if (!(ss >> board_str >> turn >> hand_str >> move_no)) return std::nullopt;

  komori::detail::SfenBoard board;
  komori::detail::ParseSfenBoard(board_str, board);

  // 非王盤上駒の座標収集
  struct PiecePos { int r, f; std::string cell; bool is_attacker; char upper_ch; };
  std::vector<PiecePos> pieces;
  for (int r = 0; r < 9; ++r)
    for (int f = 0; f < 9; ++f) {
      const std::string& c = board[r][f];
      if (c.empty()) continue;
      const char bc = c.back();
      const char uc = static_cast<char>(std::toupper(static_cast<unsigned char>(bc)));
      if (uc == 'K') continue;
      pieces.push_back({r, f, c, std::isupper(static_cast<unsigned char>(bc)), uc});
    }

  if (pieces.size() < 2) return std::nullopt;  // 2駒未満なら無意味

  int cnt = 0;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    for (std::size_t j = i + 1; j < pieces.size(); ++j) {
      const auto& pi = pieces[i];
      const auto& pj = pieces[j];
      // piとpjを盤外に出したボード
      komori::detail::SfenBoard base = board;
      base[pi.r][pi.f] = "";
      base[pj.r][pj.f] = "";

      for (int nr1 = 0; nr1 < 9; ++nr1) {
        for (int nf1 = 0; nf1 < 9; ++nf1) {
          // pi の行き所なし駒チェック
          if (pi.is_attacker && !komori::detail::IsValidBoardPlacement(pi.upper_ch, nr1)) continue;
          if (!pi.is_attacker) {
            if ((pi.upper_ch=='P'||pi.upper_ch=='L') && nr1==8) continue;
            if (pi.upper_ch=='N' && nr1>=7) continue;
          }
          if (!base[nr1][nf1].empty()) continue;  // 占有済みチェック(後でjも)

          for (int nr2 = 0; nr2 < 9; ++nr2) {
            for (int nf2 = 0; nf2 < 9; ++nf2) {
              if (nr2 == nr1 && nf2 == nf1) continue;
              // pj の行き所なし駒チェック
              if (pj.is_attacker && !komori::detail::IsValidBoardPlacement(pj.upper_ch, nr2)) continue;
              if (!pj.is_attacker) {
                if ((pj.upper_ch=='P'||pj.upper_ch=='L') && nr2==8) continue;
                if (pj.upper_ch=='N' && nr2>=7) continue;
              }
              if (!base[nr2][nf2].empty()) continue;

              komori::detail::SfenBoard perturbed = base;
              perturbed[nr1][nf1] = pi.cell;
              perturbed[nr2][nf2] = pj.cell;

              const std::string new_sfen = komori::detail::BuildSfenBoard(perturbed)
                  + " " + turn + " " + hand_str + " " + move_no;

              auto result = TryCandidate(new_sfen, verify_options, target_moves, time_limit_ms);
              if (result) {
                sync_cout << "info string [perturb2] 審美合格: "
                          << pi.upper_ch << " →" << (9-nf1) << char('a'+nr1)
                          << " " << pj.upper_ch << " →" << (9-nf2) << char('a'+nr2)
                          << " " << new_sfen << sync_endl;
                return result;
              }
              ++cnt;
              if (cnt % 500 == 0) {
                sync_cout << "info string [perturb2] 試行 " << cnt << " 候補" << sync_endl;
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                if (static_cast<std::uint64_t>(elapsed) >= budget_ms) {
                  sync_cout << "info string [perturb2] 時間切れ (" << elapsed << "ms), 打ち切り"
                            << sync_endl;
                  return std::nullopt;
                }
              }
            }
          }
        }
      }
    }
  }
  sync_cout << "info string [perturb2] 全組み合わせ試行済み: " << cnt << " 候補, 合格なし" << sync_endl;
  return std::nullopt;
}

// 攻め方の持ち駒にpiece_upperを1枚追加したhand文字列を返す
static std::string AddToAttackerHand(const std::string& hand_str, char piece_upper) {
  std::map<char, int> ahand, dhand;
  int cnt = 0;
  for (char c : hand_str) {
    if (c == '-') continue;
    if (std::isdigit((unsigned char)c)) { cnt = cnt * 10 + (c - '0'); }
    else {
      int n = cnt ? cnt : 1; cnt = 0;
      if (std::isupper((unsigned char)c)) ahand[c] += n;
      else dhand[(char)std::toupper((unsigned char)c)] += n;
    }
  }
  ahand[piece_upper]++;
  std::string result;
  for (char c : {'R','B','G','S','N','L','P'}) {
    if (ahand[c] > 0) { if (ahand[c] > 1) result += std::to_string(ahand[c]); result += c; }
  }
  for (char c : {'R','B','G','S','N','L','P'}) {
    char lc = (char)std::tolower((unsigned char)c);
    if (dhand[c] > 0) { if (dhand[c] > 1) result += std::to_string(dhand[c]); result += lc; }
  }
  return result.empty() ? "-" : result;
}

// 逆算ステップ（df-pn のみ、EV なし）: N手詰め局面 → N+2手詰め候補を返す
// 中間ステップ用。EV は最終ステップの RetrogradExtend に任せる。
// 戦略: 攻め方駒1枚を手持ちに戻し、玉を盤上の任意の空きマスへ移動（または同位置維持）
static std::optional<komori::TsumeGeneratedProblem> RetrogradStep(
    const komori::TsumeGeneratedProblem& base,
    int target_moves,
    std::uint64_t time_limit_ms,
    std::uint64_t budget_ms = 30'000) {
  const auto t0 = std::chrono::steady_clock::now();
  std::istringstream ss(base.sfen);
  std::string board_str, turn, hand_str, move_no;
  if (!(ss >> board_str >> turn >> hand_str >> move_no)) return std::nullopt;
  komori::detail::SfenBoard board{};
  komori::detail::ParseSfenBoard(board_str, board);
  int kr = -1, kf = -1;
  for (int r = 0; r < 9 && kr < 0; ++r)
    for (int f = 0; f < 9 && kr < 0; ++f)
      if (board[r][f] == "k") { kr = r; kf = f; }
  if (kr < 0) return std::nullopt;

  const auto test_candidate = [&](const std::string& new_sfen) -> bool {
    Position candidate; StateListPtr states(new StateList(1));
    candidate.set(new_sfen, &states->back(), Threads.main());
    if (!komori::ValidateTsumePosition(candidate).empty()) return false;
    if (candidate.in_check()) return false;
    Search::LimitsType limits;
    limits.mate = static_cast<int>(std::min<std::uint64_t>(time_limit_ms / 3, 500));
    Time.reset();
    Threads.start_thinking(candidate, states, limits, false);
    Threads.main()->wait_for_search_finished();
    return g_search_result == komori::NodeState::kProven &&
           static_cast<int>(Searcher().BestMoves().size()) == target_moves;
  };

  int cand_cnt = 0;

  // 駒除去なしで玉だけ移動（手数を伸ばす最もシンプルな手段）
  for (int er = 0; er < 9; ++er) {
    for (int ef = 0; ef < 9; ++ef) {
      if (er == kr && ef == kf) continue;
      if (!board[er][ef].empty()) continue;
      komori::detail::SfenBoard board2 = board;
      board2[kr][kf] = "";
      board2[er][ef] = "k";
      const std::string new_sfen = komori::detail::BuildSfenBoard(board2)
                                   + " " + turn + " " + hand_str + " " + move_no;
      if (test_candidate(new_sfen)) {
        const auto& bm = Searcher().BestMoves();
        std::vector<std::string> sol;
        for (const auto& m : bm) sol.push_back(USI::move(m));
        sync_cout << "info string [逆算ステップ合格] " << target_moves << "ply(王移動): "
                  << new_sfen << sync_endl;
        return komori::TsumeGeneratedProblem{new_sfen, target_moves, sol, ""};
      }
      ++cand_cnt;
      if (cand_cnt % 100 == 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (static_cast<std::uint64_t>(elapsed) >= budget_ms) {
          sync_cout << "info string [逆算ステップ] 時間切れ " << cand_cnt << "候補試行" << sync_endl;
          return std::nullopt;
        }
      }
    }
  }

  for (int pr = 0; pr < 9; ++pr) {
    for (int pf = 0; pf < 9; ++pf) {
      const std::string& cell = board[pr][pf];
      if (cell.empty()) continue;
      char bc = cell.back();
      if (!std::isupper((unsigned char)bc) || bc == 'K') continue;
      char hand_ch = bc;
      komori::detail::SfenBoard board2 = board;
      board2[pr][pf] = "";
      const std::string hand2 = AddToAttackerHand(hand_str, hand_ch);

      // 玉を同位置維持（駒除去のみ）
      {
        const std::string new_sfen = komori::detail::BuildSfenBoard(board2)
                                     + " " + turn + " " + hand2 + " " + move_no;
        if (test_candidate(new_sfen)) {
          const auto& bm = Searcher().BestMoves();
          std::vector<std::string> sol;
          for (const auto& m : bm) sol.push_back(USI::move(m));
          sync_cout << "info string [逆算ステップ合格] " << target_moves << "ply: "
                    << new_sfen << sync_endl;
          return komori::TsumeGeneratedProblem{new_sfen, target_moves, sol, ""};
        }
        ++cand_cnt;
      }

      // 玉を盤上の全マスへ移動（近傍から遠傍まで）
      for (int er = 0; er < 9; ++er) {
        for (int ef = 0; ef < 9; ++ef) {
          if (er == kr && ef == kf) continue;  // 同位置は上で処理済み
          if (!board2[er][ef].empty()) continue;
          komori::detail::SfenBoard board3 = board2;
          board3[kr][kf] = "";
          board3[er][ef] = "k";
          const std::string new_sfen = komori::detail::BuildSfenBoard(board3)
                                       + " " + turn + " " + hand2 + " " + move_no;
          if (test_candidate(new_sfen)) {
            const auto& bm = Searcher().BestMoves();
            std::vector<std::string> sol;
            for (const auto& m : bm) sol.push_back(USI::move(m));
            sync_cout << "info string [逆算ステップ合格] " << target_moves << "ply: "
                      << new_sfen << sync_endl;
            return komori::TsumeGeneratedProblem{new_sfen, target_moves, sol, ""};
          }
          ++cand_cnt;
          if (cand_cnt % 100 == 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (static_cast<std::uint64_t>(elapsed) >= budget_ms) {
              sync_cout << "info string [逆算ステップ] 時間切れ " << cand_cnt
                        << "候補試行" << sync_endl;
              return std::nullopt;
            }
          }
        }
      }
    }
  }
  return std::nullopt;
}

// 逆算式拡張: (target_moves-2)手詰め局面 → target_moves手詰めを探す
// 攻め方駒1枚を手持ちに戻し、玉を盤上の任意の空きマス（または同位置）に置いて試す。
// 近傍マス優先→全マスの順で探索し、合格したら TryCandidate の結果を返す。
static std::optional<komori::TsumeGeneratedProblem> RetrogradExtend(
    const komori::TsumeGeneratedProblem& base,
    const komori::tsume::VerifyOptions& opts,
    int target_moves,
    std::uint64_t time_limit_ms,
    std::uint64_t budget_ms = 120'000) {
  const auto t0 = std::chrono::steady_clock::now();

  std::istringstream ss(base.sfen);
  std::string board_str, turn, hand_str, move_no;
  if (!(ss >> board_str >> turn >> hand_str >> move_no)) return std::nullopt;

  komori::detail::SfenBoard board{};
  komori::detail::ParseSfenBoard(board_str, board);

  int kr = -1, kf = -1;
  for (int r = 0; r < 9 && kr < 0; ++r)
    for (int f = 0; f < 9 && kr < 0; ++f)
      if (board[r][f] == "k") { kr = r; kf = f; }
  if (kr < 0) return std::nullopt;

  // 攻め方盤上駒数が少なすぎる場合は延伸が現実的でない（単駒は除去後に空盤になる）
  {
    int atk_board = 0;
    for (const auto& row : board) for (const auto& cell : row) {
      if (cell.empty()) continue;
      const char bc = cell.back();
      if (std::isupper(static_cast<unsigned char>(bc)) && bc != 'K') ++atk_board;
    }
    // 手持ちも含めた総攻め方駒数を数える
    int atk_hand = 0;
    if (hand_str != "-") {
      int cnt2 = 0;
      for (char c : hand_str) {
        if (std::isdigit((unsigned char)c)) { cnt2 = cnt2 * 10 + (c - '0'); continue; }
        int n = cnt2 ? cnt2 : 1; cnt2 = 0;
        if (std::isupper((unsigned char)c)) atk_hand += n;
      }
    }
    if (atk_board + atk_hand < 2) {
      sync_cout << "info string [逆算] 攻め方駒不足(" << (atk_board+atk_hand)
                << "枚), スキップ" << sync_endl;
      return std::nullopt;
    }
  }

  int cand_cnt = 0;

  // 攻め方手持ちをクリアした sparse SFEN を返す（chain で積まれた駒を受け方予備へ）
  const auto clear_attacker_hand = [](const std::string& sparse) -> std::string {
    std::istringstream ss2(sparse);
    std::string bd, tr, hs, mn;
    if (!(ss2 >> bd >> tr >> hs >> mn)) return sparse;
    std::string dh;
    int cnt2 = 0;
    for (char c : hs) {
      if (c == '-') continue;
      if (std::isdigit((unsigned char)c)) { cnt2 = cnt2 * 10 + (c - '0'); continue; }
      if (std::islower((unsigned char)c)) {
        int n = cnt2 ? cnt2 : 1; cnt2 = 0;
        if (n > 1) dh += std::to_string(n); dh += c;
      } else { cnt2 = 0; }  // skip attacker pieces
    }
    return bd + " " + tr + " " + (dh.empty() ? "-" : dh) + " " + mn;
  };

  const auto try_39 = [&](const std::string& sparse) -> std::optional<komori::TsumeGeneratedProblem> {
    // バージョン1: chain hand pieces を攻め方持ち駒に保持したまま
    const std::string sfen39 = komori::detail::CompleteDefenderReserve(sparse, false);
    {
      Position p39; StateListPtr s39(new StateList(1));
      p39.set(sfen39, &s39->back(), Threads.main());
      if (komori::ValidateTsumePosition(p39).empty()) {
        auto r = TryCandidate(sfen39, opts, target_moves, time_limit_ms);
        if (r) return r;
      }
    }
    // バージョン2: 攻め方手持ちをクリア（surplus-hand 回避）
    const std::string sparse2 = clear_attacker_hand(sparse);
    if (sparse2 != sparse) {
      const std::string sfen39b = komori::detail::CompleteDefenderReserve(sparse2, false);
      Position p39b; StateListPtr s39b(new StateList(1));
      p39b.set(sfen39b, &s39b->back(), Threads.main());
      if (komori::ValidateTsumePosition(p39b).empty()) {
        return TryCandidate(sfen39b, opts, target_moves, time_limit_ms);
      }
    }
    return std::nullopt;
  };

  const auto check_budget = [&]() -> bool {
    if (cand_cnt % 100 != 0) return true;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (static_cast<std::uint64_t>(elapsed) >= budget_ms) {
      sync_cout << "info string [逆算] 時間切れ " << cand_cnt << "候補試行" << sync_endl;
      return false;
    }
    return true;
  };

  // 駒除去なしで玉だけ移動（board 上の全空きマス）
  for (int er = 0; er < 9; ++er) {
    for (int ef = 0; ef < 9; ++ef) {
      if (er == kr && ef == kf) continue;
      if (!board[er][ef].empty()) continue;
      komori::detail::SfenBoard board2 = board;
      board2[kr][kf] = "";
      board2[er][ef] = "k";
      const std::string sparse = komori::detail::BuildSfenBoard(board2)
                                 + " " + turn + " " + hand_str + " " + move_no;
      auto result = try_39(sparse);
      ++cand_cnt;
      if (result) {
        sync_cout << "info string [逆算合格(王移動)] " << target_moves << "ply: "
                  << result->sfen << sync_endl;
        return result;
      }
      if (!check_budget()) return std::nullopt;
    }
  }

  // 盤上の攻め方駒(大文字、王以外)を1枚ずつ受け方予備駒として除去
  for (int pr = 0; pr < 9; ++pr) {
    for (int pf = 0; pf < 9; ++pf) {
      const std::string& cell = board[pr][pf];
      if (cell.empty()) continue;
      char bc = cell.back();
      if (!std::isupper((unsigned char)bc) || bc == 'K') continue;

      komori::detail::SfenBoard board2 = board;
      board2[pr][pf] = "";
      // 駒は攻め方手持ちに加えず CompleteDefenderReserve で受け方予備として扱う

      // 玉を同位置維持（駒除去のみ）
      {
        const std::string sparse = komori::detail::BuildSfenBoard(board2)
                                   + " " + turn + " " + hand_str + " " + move_no;
        auto result = try_39(sparse);
        ++cand_cnt;
        if (result) {
          sync_cout << "info string [逆算合格] " << target_moves << "ply: "
                    << result->sfen << sync_endl;
          return result;
        }
        if (!check_budget()) return std::nullopt;
      }

      // 玉を近傍8マスへ移動（優先探索）
      for (int dr = -1; dr <= 1; ++dr) {
        for (int df = -1; df <= 1; ++df) {
          if (!dr && !df) continue;
          const int er = kr + dr, ef = kf + df;
          if (er < 0 || er >= 9 || ef < 0 || ef >= 9) continue;
          if (board2[er][ef].empty()) {
            komori::detail::SfenBoard board3 = board2;
            board3[kr][kf] = "";
            board3[er][ef] = "k";
            const std::string sparse = komori::detail::BuildSfenBoard(board3)
                                       + " " + turn + " " + hand_str + " " + move_no;
            auto result = try_39(sparse);
            ++cand_cnt;
            if (result) {
              sync_cout << "info string [逆算合格] " << target_moves << "ply: "
                        << result->sfen << sync_endl;
              return result;
            }
            if (!check_budget()) return std::nullopt;
          }
        }
      }

      // 玉を全マスへ移動（近傍で見つからない場合の網羅探索）
      for (int er = 0; er < 9; ++er) {
        for (int ef = 0; ef < 9; ++ef) {
          if (er == kr && ef == kf) continue;  // 同位置は上で処理済み
          if (std::abs(er - kr) <= 1 && std::abs(ef - kf) <= 1) continue;
          if (board2[er][ef].empty()) {
            komori::detail::SfenBoard board3 = board2;
            board3[kr][kf] = "";
            board3[er][ef] = "k";
            const std::string sparse = komori::detail::BuildSfenBoard(board3)
                                       + " " + turn + " " + hand_str + " " + move_no;
            auto result = try_39(sparse);
            ++cand_cnt;
            if (result) {
              sync_cout << "info string [逆算合格] " << target_moves << "ply: "
                        << result->sfen << sync_endl;
              return result;
            }
            if (!check_budget()) return std::nullopt;
          }
        }
      }
    }
  }

  // 攻め方盤上駒を別マスに移動（reposition）して target_moves 手詰めを探す
  // 駒除去とは異なり攻め方の戦力は変わらない（配置のみ変更）
  for (int pr = 0; pr < 9; ++pr) {
    for (int pf = 0; pf < 9; ++pf) {
      const std::string& cell = board[pr][pf];
      if (cell.empty()) continue;
      char bc = cell.back();
      if (!std::isupper((unsigned char)bc) || bc == 'K') continue;

      komori::detail::SfenBoard board2 = board;
      board2[pr][pf] = "";

      // 移動先: 王から4マス以内に優先して配置（遠距離移動は攻撃力が変わりすぎる）
      for (int nr = std::max(0, kr - 4); nr <= std::min(8, kr + 4); ++nr) {
        for (int nf = std::max(0, kf - 4); nf <= std::min(8, kf + 4); ++nf) {
          if (nr == pr && nf == pf) continue;
          if (!board2[nr][nf].empty()) continue;
          if (!komori::detail::IsValidBoardPlacement(bc, nr)) continue;

          komori::detail::SfenBoard board3 = board2;
          board3[nr][nf] = cell;
          const std::string sparse = komori::detail::BuildSfenBoard(board3)
                                     + " " + turn + " " + hand_str + " " + move_no;
          auto result = try_39(sparse);
          ++cand_cnt;
          if (result) {
            sync_cout << "info string [逆算合格(駒移動)] " << target_moves << "ply: "
                      << result->sfen << sync_endl;
            return result;
          }
          if (!check_budget()) return std::nullopt;
        }
      }
    }
  }

  sync_cout << "info string [逆算] " << cand_cnt << "候補試行, 合格なし" << sync_endl;
  return std::nullopt;
}

static std::optional<komori::TsumeGeneratedProblem> PerturbAndRetry(
    const komori::TsumeGeneratedProblem& base_problem,
    const komori::tsume::VerifyOptions& verify_options,
    int target_moves,
    std::uint64_t time_limit_ms) {
  std::istringstream ss(base_problem.sfen);
  std::string board_str, turn, hand_str, move_no;
  if (!(ss >> board_str >> turn >> hand_str >> move_no)) return std::nullopt;

  komori::detail::SfenBoard board;
  komori::detail::ParseSfenBoard(board_str, board);

  int cnt_dfpn = 0, cnt_ev = 0, cnt_no_unnec = 0, cnt_aesthetic = 0;
  for (int r = 0; r < 9; ++r) {
    for (int f = 0; f < 9; ++f) {
      const std::string& cell = board[r][f];
      if (cell.empty()) continue;
      const char base_ch = cell.back();
      const char upper_ch = static_cast<char>(std::toupper(static_cast<unsigned char>(base_ch)));
      if (upper_ch == 'K') continue;  // 玉は動かさない

      const bool is_attacker = std::isupper(static_cast<unsigned char>(base_ch));

      for (int nr = 0; nr < 9; ++nr) {
        for (int nf = 0; nf < 9; ++nf) {
          if (nr == r && nf == f) continue;
          if (!board[nr][nf].empty()) continue;  // 占有済み

          // 行き所なし駒チェック
          if (is_attacker && !komori::detail::IsValidBoardPlacement(upper_ch, nr)) continue;
          if (!is_attacker) {
            if ((upper_ch == 'P' || upper_ch == 'L') && nr == 8) continue;
            if (upper_ch == 'N' && nr >= 7) continue;
          }

          komori::detail::SfenBoard perturbed = board;
          perturbed[nr][nf] = cell;
          perturbed[r][f] = "";

          const std::string new_sfen = komori::detail::BuildSfenBoard(perturbed)
              + " " + turn + " " + hand_str + " " + move_no;

          Position candidate; StateListPtr states(new StateList(1));
          candidate.set(new_sfen, &states->back(), Threads.main());
          if (!komori::ValidateTsumePosition(candidate).empty()) continue;
          if (candidate.in_check()) continue;

          Search::LimitsType limits;
          limits.mate = static_cast<int>(time_limit_ms / 3);
          Time.reset();
          Threads.start_thinking(candidate, states, limits, false);
          Threads.main()->wait_for_search_finished();

          if (g_search_result != komori::NodeState::kProven) continue;
          const auto& bm = Searcher().BestMoves();
          if (static_cast<int>(bm.size()) != target_moves) continue;
          if (komori::tsume::HasSurplusAttackerHand(candidate, bm)) continue;
          ++cnt_dfpn;
          sync_cout << "info string [perturb] df-pn合格: " << cell << " "
                    << (9 - f) << char('a' + r) << "→" << (9 - nf) << char('a' + nr)
                    << " " << new_sfen << sync_endl;

          // 高速EV事前フィルタ: full EVの前に軽量チェック
          {
            komori::tsume::VerifyOptions quick_opts;
            quick_opts.max_ply = target_moves;
            quick_opts.max_nodes = std::max<std::uint64_t>(1'500'000ULL,
                komori::detail::ComputeNodesLimit(target_moves));
            quick_opts.double_king = false;
            Position qp; StateListPtr qs(new StateList(1));
            qp.set(new_sfen, &qs->back(), Threads.main());
            komori::tsume::ExhaustiveVerifier qev(quick_opts);
            const auto qr = qev.Run(qp);
            if (qr.complete && (!qr.unique || qr.proof != komori::tsume::Proof::kMate ||
                                qr.mate_ply != target_moves)) {
              sync_cout << "info string [perturb] quick-EV却下" << sync_endl;
              continue;
            }
          }

          Position verify_pos; StateListPtr vs(new StateList(1));
          verify_pos.set(new_sfen, &vs->back(), Threads.main());
          komori::tsume::ExhaustiveVerifier ev(verify_options);
          auto result = ev.Run(verify_pos);
          if (komori::tsume::HasSurplusAttackerHand(verify_pos, result.principal)) continue;
          // complete=0 かつ ply==target_moves の場合、ノード不足の可能性 → 3倍ノードで再試行
          if (!result.complete && result.mate_ply == target_moves) {
            sync_cout << "info string [perturb] ノード不足再試行(3x): " << new_sfen << sync_endl;
            komori::tsume::VerifyOptions retry_opts = verify_options;
            retry_opts.max_nodes = verify_options.max_nodes * 3;
            Position retry_pos; StateListPtr rs(new StateList(1));
            retry_pos.set(new_sfen, &rs->back(), Threads.main());
            komori::tsume::ExhaustiveVerifier ev2(retry_opts);
            result = ev2.Run(retry_pos);
            if (komori::tsume::HasSurplusAttackerHand(retry_pos, result.principal)) continue;
          }
          if (!result.complete || result.proof != komori::tsume::Proof::kMate ||
              !result.unique || result.mate_ply != target_moves) {
            sync_cout << "info string [perturb] full-EV却下 complete=" << result.complete
                      << " unique=" << result.unique << " ply=" << result.mate_ply << sync_endl;
            continue;
          }
          ++cnt_ev;

          AnalyzeUnnecessaryPieces(new_sfen, verify_options, result);
          if (result.unnecessary_piece) {
            sync_cout << "info string [perturb] 邪魔駒却下" << sync_endl;
            continue;
          }
          ++cnt_no_unnec;

          auto record = komori::tsume::MakeRecord(verify_pos, new_sfen, verify_options,
                                                  std::move(result));
          if (!record.aesthetic_pass) {
            sync_cout << "info string [perturb] 審美却下: "
                      << (record.aesthetic_reasons.empty() ? "?" : record.aesthetic_reasons[0])
                      << " hendou=" << record.verification.hendou_count << sync_endl;
            continue;
          }
          ++cnt_aesthetic;

          komori::TsumeGeneratedProblem prob;
          prob.sfen = new_sfen;
          prob.mate_in = target_moves;
          for (Move m : record.verification.principal) prob.solution.push_back(USI::move(m));
          prob.record_json = komori::tsume::ToJson(record, verify_pos);
          sync_cout << "info string [perturb] 審美合格: " << cell << " "
                    << (9 - f) << char('a' + r) << "→" << (9 - nf) << char('a' + nr)
                    << " " << new_sfen << sync_endl;
          return prob;
        }
      }
    }
  }
  sync_cout << "info string [perturb] 結果: df-pn合格=" << cnt_dfpn
            << " EV合格=" << cnt_ev << " 邪魔駒なし=" << cnt_no_unnec
            << " 審美合格=" << cnt_aesthetic << sync_endl;
  return std::nullopt;
}

// SFENのボード部分を左右反転する（将棋盤の左右対称局面を同一視するため）
static std::string MirrorSfen(const std::string& sfen) {
  const auto space_pos = sfen.find(' ');
  const std::string board_str = sfen.substr(0, space_pos);
  const std::string rest = (space_pos != std::string::npos) ? sfen.substr(space_pos) : "";
  std::string mirrored;
  bool first_rank = true;
  for (size_t i = 0; i <= board_str.size(); ) {
    size_t j = board_str.find('/', i);
    if (j == std::string::npos) j = board_str.size();
    const std::string rank = board_str.substr(i, j - i);
    std::vector<std::string> pieces;
    for (size_t k = 0; k < rank.size(); ) {
      if (std::isdigit((unsigned char)rank[k])) {
        for (int m = 0; m < rank[k] - '0'; ++m) pieces.push_back(".");
        ++k;
      } else if (rank[k] == '+') {
        pieces.push_back("+" + std::string(1, rank[k + 1]));
        k += 2;
      } else {
        pieces.push_back(std::string(1, rank[k]));
        ++k;
      }
    }
    std::reverse(pieces.begin(), pieces.end());
    if (!first_rank) mirrored += '/';
    first_rank = false;
    int empty = 0;
    for (const auto& p : pieces) {
      if (p == ".") { ++empty; }
      else { if (empty) { mirrored += char('0' + empty); empty = 0; } mirrored += p; }
    }
    if (empty) mirrored += char('0' + empty);
    i = j + 1;
  }
  return mirrored + rest;
}

void GenerateProblemsForMoves(int target_moves, int count,
                               std::mt19937& rng,
                               std::vector<komori::TsumeGeneratedProblem>& found,
                               const std::string& output_file = "") {
  // 生成ループ用: 小さい TT + silent で Init（大 TT だと初回アクセス時のページフォルトで詰まる）
  komori::EngineOption gen_option = g_option;
  gen_option.hash_mb              = 64;
  gen_option.silent               = true;
  gen_option.pv_interval          = std::numeric_limits<std::uint64_t>::max() / 2;
  Searcher().Init(gen_option, static_cast<std::uint32_t>(Threads.size()));

  const int target_count  = static_cast<int>(found.size()) + count;
  const int max_attempts  = count * 10000;

  // 手数に応じた探索時間上限（ms）
  const auto time_limit_ms = static_cast<std::uint64_t>(komori::detail::ComputeTimeLimitMs(target_moves));

  // 重複除去: 既に発見済みのSFEN集合（左右対称局面も同一視）
  std::unordered_set<std::string> seen_sfens;
  auto seen_insert = [&](const std::string& s) {
    seen_sfens.insert(s);
    seen_sfens.insert(MirrorSfen(s));
  };
  for (const auto& p : found) seen_insert(p.sfen);

  // ウォームシード: 9手詰め39枚制では守方手持ちが35枚になり全候補が変同必発。スキップ。

  for (int attempt = 0; attempt < max_attempts && static_cast<int>(found.size()) < target_count;
       ++attempt) {
    if (attempt > 0 && attempt % 500 == 0)
      sync_cout << "info string [tsume_generate] 試行 " << attempt << "/" << max_attempts
                << " 発見済み " << (static_cast<int>(found.size()) - target_count + count)
                << "/" << count << sync_endl;
    // 候補局面を生成: 9手以上は30%の確率で構造化シード（N+S+B_hand パターン）を使用
    const std::string raw_sfen = [&]() -> std::string {
      if (target_moves >= 9) {
        std::bernoulli_distribution use_template(0.30);
        if (use_template(rng)) return komori::detail::GenerateStructuredSeed9ply(rng);
      }
      return komori::detail::GenerateCandidateSfen(rng, target_moves);
    }();

    Position gen_pos;
    StateListPtr st(new StateList(1));
    gen_pos.set(raw_sfen, &st->back(), Threads.main());

    // 詰将棋ルール検証: 受け方の玉が盤上にある & 出題局面で王手でない
    if (!komori::ValidateTsumePosition(gen_pos).empty()) continue;
    if (gen_pos.in_check()) continue;

    // 事前フィルタ: 玉の逃げ道と攻め方の脅威を静的評価（df-pn 前の高速スキップ）
    {
      std::istringstream pss(raw_sfen);
      std::string pboard, pturn, phand;
      pss >> pboard >> pturn >> phand;
      komori::detail::SfenBoard pb;
      komori::detail::ParseSfenBoard(pboard, pb);
      int kr = -1, kf = -1;
      for (int r = 0; r < 9 && kr < 0; ++r)
        for (int f = 0; f < 9 && kr < 0; ++f)
          if (pb[r][f] == "k") { kr = r; kf = f; }
      bool prefilter_ok = false;
      if (kr >= 0) {
        // 攻め方の駒が玉に近いか、長距離駒か（盤上）
        for (int r = 0; r < 9 && !prefilter_ok; ++r)
          for (int f = 0; f < 9 && !prefilter_ok; ++f) {
            const std::string& cell = pb[r][f];
            if (cell.empty()) continue;
            const char ch = cell.back();
            if (!std::isupper(static_cast<unsigned char>(ch))) continue;
            const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (up == 'R' || up == 'B') {
              prefilter_ok = true;  // 飛角は常に脅威
            } else {
              const int dist = std::abs(r - kr) + std::abs(f - kf);
              if (dist <= 3) prefilter_ok = true;
            }
          }
        // 持ち駒があれば打ち込み可能
        if (!prefilter_ok && phand != "-")
          for (char c : phand)
            if (std::isupper(static_cast<unsigned char>(c))) { prefilter_ok = true; break; }
      }
      if (!prefilter_ok) continue;

      // 最初から詰みそうなフィルター: 玉の逃げ道が1マス以下なら配置が自明すぎる
      if (kr >= 0) {
        int free_neighbors = 0;
        for (int dr = -1; dr <= 1; ++dr)
          for (int df2 = -1; df2 <= 1; ++df2) {
            if (dr == 0 && df2 == 0) continue;
            const int nr2 = kr + dr, nf2 = kf + df2;
            if (nr2 < 0 || nr2 >= 9 || nf2 < 0 || nf2 >= 9) continue;
            if (pb[nr2][nf2].empty()) ++free_neighbors;
          }
        if (free_neighbors <= 1) continue;
      }
    }

    // 詰み探索: limits.mate を時間上限(ms)として使用（KomoringHeights の慣習）
    Search::LimitsType limits;
    limits.mate = static_cast<int>(time_limit_ms);

    Time.reset();
    Threads.start_thinking(gen_pos, st, limits, false);
    Threads.main()->wait_for_search_finished();

    if (g_search_result == komori::NodeState::kProven) {
      const auto& best_moves_raw = Searcher().BestMoves();
      const int mate_in_raw      = static_cast<int>(best_moves_raw.size());

      sync_cout << "info string [small-hand hit] " << mate_in_raw << "ply " << raw_sfen << sync_endl;
      // 逆算式: 3以上の奇数手詰め（target以下、target と同じ偶奇）を受け入れる
      if (mate_in_raw < 3 || mate_in_raw > target_moves
          || (target_moves - mate_in_raw) % 2 != 0) continue;

      // 低審美確率制御: 攻め方に駒打ちが一切ない場合、確率を下げる（禁止ではない）
      if (mate_in_raw >= 5) {
        bool attacker_has_drop = false;
        for (int mi = 0; mi < mate_in_raw; mi += 2)
          if (is_drop(best_moves_raw[mi])) { attacker_has_drop = true; break; }
        if (!attacker_has_drop && rng() % 5 != 0) continue;  // 20%に確率削減
      }

      // 逆算式チェーン候補 (mate_in_raw < target_moves):
      // 39枚制の事前確認は不要（失敗しても逆算チェーンへ進む）
      if (mate_in_raw < target_moves) {
        komori::tsume::VerifyOptions retro_opts;
        retro_opts.max_ply = target_moves;
        retro_opts.max_nodes = komori::detail::ComputeNodesLimit(target_moves);
        retro_opts.double_king = false;

        // 39枚制が既に target_moves 手詰め → ショートカット (直接 TryCandidate)
        {
          const std::string sfen39_fast = komori::detail::CompleteDefenderReserve(raw_sfen, false);
          Position pos39_fast; StateListPtr st39_fast(new StateList(1));
          pos39_fast.set(sfen39_fast, &st39_fast->back(), Threads.main());
          if (komori::ValidateTsumePosition(pos39_fast).empty()) {
            Search::LimitsType lim39_fast; lim39_fast.mate = static_cast<int>(time_limit_ms);
            Searcher().Init(gen_option, static_cast<std::uint32_t>(Threads.size()));
            Time.reset();
            Threads.start_thinking(pos39_fast, st39_fast, lim39_fast, false);
            Threads.main()->wait_for_search_finished();
            if (g_search_result == komori::NodeState::kProven &&
                static_cast<int>(Searcher().BestMoves().size()) == target_moves) {
              sync_cout << "info string [39枚変換] " << raw_sfen << " → " << sfen39_fast
                        << " (" << target_moves << "手)" << sync_endl;
              sync_cout << "info string [39枚直接候補] " << target_moves << "ply: " << sfen39_fast << sync_endl;
              if (seen_sfens.count(sfen39_fast)) { continue; }
              auto result = TryCandidate(sfen39_fast, retro_opts, target_moves, time_limit_ms * 3);
              if (result && seen_sfens.find(result->sfen) == seen_sfens.end()) {
                seen_insert(result->sfen);
                found.push_back(*result);
                sync_cout << "info string [" << target_moves << "手詰め] 発見(39枚直接) "
                          << static_cast<int>(found.size()) << "/" << count
                          << ": " << result->sfen << sync_endl;
                if (!output_file.empty()) komori::SaveTsumeProblems({*result}, output_file);
                if (static_cast<int>(found.size()) >= target_count) break;
                continue;  // 成功 → 次の候補へ
              }
              // TryCandidate 失敗 → 逆算チェーンへ fall through
            }
          }
        }

        // 逆算チェーン: スパース局面(raw_sfen)から延伸
        std::vector<std::string> raw_base_sol;
        for (const auto& m : best_moves_raw) raw_base_sol.push_back(USI::move(m));
        komori::TsumeGeneratedProblem cur_prob{raw_sfen, mate_in_raw, raw_base_sol, ""};
        sync_cout << "info string [逆算開始] " << mate_in_raw << "手詰め(スパース) → "
                  << target_moves << "手詰めへ: " << raw_sfen << sync_endl;

        // 中間ステップ: df-pn のみ（EV なし）で延伸
        bool chain_ok = true;
        for (int ply = mate_in_raw + 2; ply <= target_moves - 2; ply += 2) {
          auto step = RetrogradStep(cur_prob, ply, time_limit_ms, 30'000);
          if (!step) { chain_ok = false; break; }
          cur_prob = *step;
          sync_cout << "info string [逆算チェーン] " << ply << "手: " << cur_prob.sfen << sync_endl;
        }
        if (!chain_ok) continue;

        // 最終ステップ: RetrogradExtend（try_39 内で39枚変換済み）
        g_aesthetic_near_miss.clear();  // 前の試行の残りをクリア
        auto extended = RetrogradExtend(cur_prob, retro_opts, target_moves, time_limit_ms);
        if (extended) {
          // surplus-strip で手持ちが変わった可能性があるため再度 CompleteDefenderReserve を適用
          const std::string sfen39_chain = komori::detail::CompleteDefenderReserve(extended->sfen, false);
          {
            Position p39c; StateListPtr st39c(new StateList(1));
            p39c.set(sfen39_chain, &st39c->back(), Threads.main());
            if (komori::ValidateTsumePosition(p39c).empty()) {
              Search::LimitsType lim39c; lim39c.mate = static_cast<int>(time_limit_ms);
              Searcher().Init(gen_option, static_cast<std::uint32_t>(Threads.size()));
              Time.reset();
              Threads.start_thinking(p39c, st39c, lim39c, false);
              Threads.main()->wait_for_search_finished();
              if (g_search_result == komori::NodeState::kProven &&
                  static_cast<int>(Searcher().BestMoves().size()) == target_moves) {
                extended->solution.clear();
                for (Move m : Searcher().BestMoves()) extended->solution.push_back(USI::move(m));
                if (seen_sfens.count(extended->sfen)) continue;
                seen_insert(extended->sfen);
                found.push_back(*extended);
                sync_cout << "info string [" << target_moves << "手詰め] 発見(逆算チェーン) "
                          << static_cast<int>(found.size()) << "/" << count
                          << ": " << extended->sfen << sync_endl;
                if (!output_file.empty()) komori::SaveTsumeProblems({*extended}, output_file);
                if (static_cast<int>(found.size()) >= target_count) break;
              }
            }
          }
        }
        // 審美惜しく不合格の近傍候補があれば PerturbAndRetry で変同除去を試みる
        if (g_aesthetic_near_miss.valid() && g_aesthetic_near_miss.target_moves == target_moves) {
          sync_cout << "info string [近傍摂動] 審美近傍候補(hendou="
                    << g_aesthetic_near_miss.hendou_count << "): "
                    << g_aesthetic_near_miss.sfen << sync_endl;
          komori::TsumeGeneratedProblem nm_prob;
          nm_prob.sfen     = g_aesthetic_near_miss.sfen;
          nm_prob.mate_in  = target_moves;
          nm_prob.solution = g_aesthetic_near_miss.solution;
          g_aesthetic_near_miss.clear();
          auto perturbed = PerturbAndRetry(nm_prob, retro_opts, target_moves, time_limit_ms);
          if (perturbed && !seen_sfens.count(perturbed->sfen)) {
            seen_insert(perturbed->sfen);
            found.push_back(*perturbed);
            sync_cout << "info string [" << target_moves << "手詰め] 発見(摂動) "
                      << static_cast<int>(found.size()) << "/" << count
                      << ": " << perturbed->sfen << sync_endl;
            if (!output_file.empty()) komori::SaveTsumeProblems({*perturbed}, output_file);
            if (static_cast<int>(found.size()) >= target_count) break;
          }
        } else {
          g_aesthetic_near_miss.clear();
        }
        sync_cout << "info string [逆算] " << mate_in_raw << "手詰め種→失敗: " << raw_sfen << sync_endl;
        continue;
      }

      // mate_in_raw == target_moves: 39枚制に変換してdf-pn再確認
      const std::string sfen39 = komori::detail::CompleteDefenderReserve(raw_sfen, false);
      {
        Position pos39; StateListPtr st39(new StateList(1));
        pos39.set(sfen39, &st39->back(), Threads.main());
        if (!komori::ValidateTsumePosition(pos39).empty()) continue;
        Search::LimitsType lim39; lim39.mate = static_cast<int>(time_limit_ms);
        Searcher().Init(gen_option, static_cast<std::uint32_t>(Threads.size()));  // TT汚染防止
        Time.reset();
        Threads.start_thinking(pos39, st39, lim39, false);
        Threads.main()->wait_for_search_finished();
        if (g_search_result != komori::NodeState::kProven) continue;
        const int mate_in_39 = static_cast<int>(Searcher().BestMoves().size());
        if (mate_in_39 != target_moves) continue;
        sync_cout << "info string [39枚変換] " << raw_sfen << " → " << sfen39
                  << " (" << mate_in_39 << "手)" << sync_endl;
      }

      const std::string& sfen = sfen39;
      Position pos39_ev; StateListPtr st39_ev(new StateList(1));
      pos39_ev.set(sfen, &st39_ev->back(), Threads.main());
      const auto& best_moves = Searcher().BestMoves();
      const int mate_in      = static_cast<int>(best_moves.size());

      sync_cout << "info string [39枚候補] " << mate_in << " ply: " << sfen << sync_endl;

      if (mate_in == target_moves) {
        // 既にsfen39で target_moves 手詰め確認済み → 直接 TryCandidate（審美もsfen39で評価）
        sync_cout << "info string [39枚直接候補2] " << mate_in << " ply: " << sfen << sync_endl;
        if (seen_sfens.count(sfen)) continue;  // 既知SFEN: EV不要でスキップ
        {
          komori::tsume::VerifyOptions opts39;
          opts39.max_ply = target_moves;
          opts39.max_nodes = komori::detail::ComputeNodesLimit(target_moves);
          opts39.double_king = false;
          // sfen39でdf-pn確認済みのため3倍時間を渡す（TryCandidate内でtime/3使用のため）
          auto result = TryCandidate(sfen, opts39, target_moves, time_limit_ms * 3);
          if (!result || seen_sfens.count(result->sfen)) continue;
          seen_insert(result->sfen);
          found.push_back(*result);
          sync_cout << "info string [" << target_moves << "手詰め] 発見(直接39枚) "
                    << static_cast<int>(found.size()) << "/" << count
                    << ": " << result->sfen << sync_endl;
          if (!output_file.empty()) komori::SaveTsumeProblems({*result}, output_file);
          if (static_cast<int>(found.size()) >= target_count) break;
        }
        // Legacy code below (unreachable, kept for reference):
        if (false) {
        std::vector<std::string> sol;
        for (const auto& m : best_moves) sol.push_back(USI::move(m));
        komori::TsumeGeneratedProblem problem{sfen, mate_in, sol, ""};

        const komori::TsumeGeneratedProblem seed_problem = problem;

        komori::tsume::VerifyOptions verify_options;
        verify_options.max_ply = target_moves;
        verify_options.max_nodes = std::max<std::uint64_t>(40'000'000ULL,
            komori::detail::ComputeNodesLimit(target_moves));
        verify_options.double_king = false;

        // 類似形探索ヘルパー: 1駒移動 → 2駒同時移動 の順で試して found に追加
        auto try_local_search = [&](const char* reason) -> bool {
          sync_cout << "info string [local_search] " << reason
                    << " seed=" << seed_problem.sfen << sync_endl;
          auto ls = PerturbAndRetry(seed_problem, verify_options, target_moves, time_limit_ms);
          if (ls) {
            found.push_back(*ls);
            sync_cout << "info string [" << target_moves << "手詰め] 発見(1駒類似形) "
                      << (static_cast<int>(found.size()) - target_count + count)
                      << "/" << count << ": " << ls->sfen << sync_endl;
            if (!output_file.empty()) komori::SaveTsumeProblems({*ls}, output_file);
            return true;
          }
          sync_cout << "info string [local_search] 1駒近傍なし、2駒摂動を試みる" << sync_endl;
          auto ls2 = PerturbTwoPiece(seed_problem, verify_options, target_moves, time_limit_ms);
          if (ls2) {
            found.push_back(*ls2);
            sync_cout << "info string [" << target_moves << "手詰め] 発見(2駒類似形) "
                      << (static_cast<int>(found.size()) - target_count + count)
                      << "/" << count << ": " << ls2->sfen << sync_endl;
            if (!output_file.empty()) komori::SaveTsumeProblems({*ls2}, output_file);
            return true;
          }
          sync_cout << "info string [local_search] 近傍なし" << sync_endl;
          return false;
        };

        // 目標手数の生候補を失わないよう、延長より先に完全検証する。
        // 従来は未検証の延長候補で元の9手候補を上書きしていた。
        sync_cout << "info string [raw candidate] " << mate_in << " ply: " << sfen << sync_endl;
        // strip_verified_necessary=true → 持ち駒除去でdf-pn失敗=変化に必要=piece_surplusは偽陽性
        bool strip_verified_necessary = false;
        if (komori::tsume::HasSurplusAttackerHand(gen_pos, best_moves)) {
          // 余剰持ち駒を取り除いて受け方の予備に戻し、局面を再設定する
          const std::string stripped = StripSurplusAttackerHand(sfen, gen_pos, best_moves);
          if (stripped.empty() || stripped == sfen) {
            // 39枚制では剥ぎ取り失敗は正常（変化に使われる場合）→ 元局面のままEVに委ねる
            strip_verified_necessary = true;
          } else {
            st.reset(new StateList(1));
            gen_pos.set(stripped, &st->back(), Threads.main());
            if (!komori::ValidateTsumePosition(gen_pos).empty() || gen_pos.in_check()) {
              // 39枚制では剥ぎ取り後の位置が不正になる場合も→元局面で進める
              strip_verified_necessary = true;
              st.reset(new StateList(1));
              gen_pos.set(sfen, &st->back(), Threads.main());
            } else {
              // 改めて df-pn で同手数確認
              Search::LimitsType retry_limits; retry_limits.mate = static_cast<int>(time_limit_ms / 2);
              Time.reset();
              Threads.start_thinking(gen_pos, st, retry_limits, false);
              Threads.main()->wait_for_search_finished();
              if (g_search_result != komori::NodeState::kProven) {
                // 剥ぎ取り後に詰まなくなった → 持ち駒が変化に必要。EV の surplus 判定を信用しない。
                sync_cout << "info string [strip] re-mate failed - falling back to original pos for EV" << sync_endl;
                strip_verified_necessary = true;
                st.reset(new StateList(1));
                gen_pos.set(sfen, &st->back(), Threads.main());
                // problem.sfen/solution は既に元の値のまま
              } else {
                const std::vector<Move> retry_moves = Searcher().BestMoves();
                if (static_cast<int>(retry_moves.size()) != target_moves) {
                  sync_cout << "info string [candidate rejected] surplus stripped mate_in changed" << sync_endl;
                  continue;
                }
                if (komori::tsume::HasSurplusAttackerHand(gen_pos, retry_moves)) {
                  // 剥ぎ取り後もまだ余剰（取った駒が手に残った等）→ EV に委ねる
                  sync_cout << "info string [strip] surplus remains - falling back to original pos for EV" << sync_endl;
                  st.reset(new StateList(1));
                  gen_pos.set(sfen, &st->back(), Threads.main());
                  // problem.sfen/solution は既に元の値のまま
                } else {
                  // 剥ぎ取り成功 — 以降は stripped の位置を使用
                  problem.sfen = stripped;
                  problem.solution.clear();
                  for (const auto& m : retry_moves) problem.solution.push_back(USI::move(m));
                  sync_cout << "info string [strip ok] " << stripped << sync_endl;
                }
              }
            }
          }
        }

        // 邪魔駒除去前の簡易EV: 手数・限定性の早期チェック（高コストな邪魔駒除去前に棄却）
        {
          komori::tsume::VerifyOptions quick_opts;
          quick_opts.max_ply = problem.mate_in;
          quick_opts.max_nodes = std::max<std::uint64_t>(1'500'000ULL,
              komori::detail::ComputeNodesLimit(problem.mate_in));
          quick_opts.double_king = false;
          Position quick_pos; StateListPtr quick_st(new StateList(1));
          quick_pos.set(problem.sfen, &quick_st->back(), Threads.main());
          komori::tsume::ExhaustiveVerifier quick_ev(quick_opts);
          const auto quick_result = quick_ev.Run(quick_pos);
          if (quick_result.complete &&
              (quick_result.proof != komori::tsume::Proof::kMate ||
               !quick_result.unique ||
               quick_result.mate_ply != problem.mate_in)) {
            sync_cout << "info string [candidate rejected quick-ev] unique="
                      << quick_result.unique << " matePly=" << quick_result.mate_ply
                      << " surplus=" << quick_result.piece_surplus << sync_endl;
            continue;
          }
        }

        // 邪魔駒確認: 不要な駒を除去して局面を簡潔に
        problem = RemoveUnnecessaryPieces(problem, time_limit_ms);

        // 邪魔駒除去後に再び余剰持ち駒が生まれた場合は改めて剥ぎ取る
        {
          st.reset(new StateList(1));
          gen_pos.set(problem.sfen, &st->back(), Threads.main());
          // 解の手順を盤面を進めながら変換（手番が交互なので根局面では全手変換不可）
          std::vector<Move> sol_moves;
          std::vector<StateInfo> tmp_states(problem.solution.size());
          {
            Position tmp; StateListPtr tmp_st(new StateList(1));
            tmp.set(problem.sfen, &tmp_st->back(), Threads.main());
            bool ok = true;
            for (std::size_t si = 0; si < problem.solution.size(); ++si) {
              const Move mv = USI::to_move(tmp, problem.solution[si]);
              if (mv == MOVE_NONE) { ok = false; break; }
              sol_moves.push_back(mv);
              tmp.do_move(mv, tmp_states[si]);
            }
            if (!ok) sol_moves.clear();
          }
          if (!sol_moves.empty()) {
            if (komori::tsume::HasSurplusAttackerHand(gen_pos, sol_moves)) {
              const std::string stripped2 = StripSurplusAttackerHand(problem.sfen, gen_pos, sol_moves);
              if (!stripped2.empty() && stripped2 != problem.sfen) {
                gen_pos.set(stripped2, &st->back(), Threads.main());
                if (komori::ValidateTsumePosition(gen_pos).empty() && !gen_pos.in_check()) {
                  Search::LimitsType lim2; lim2.mate = static_cast<int>(time_limit_ms / 2);
                  Time.reset();
                  Threads.start_thinking(gen_pos, st, lim2, false);
                  Threads.main()->wait_for_search_finished();
                  if (g_search_result == komori::NodeState::kProven) {
                    const auto& bm2 = Searcher().BestMoves();
                    if (static_cast<int>(bm2.size()) == target_moves) {
                      problem.sfen = stripped2;
                      problem.solution.clear();
                      for (const auto& m : bm2) problem.solution.push_back(USI::move(m));
                      sync_cout << "info string [post-strip ok] " << stripped2 << sync_endl;
                    }
                  }
                }
              }
            }
          }
        }

        // 短手詰め事前チェック: target_moves より短い詰みがあれば早期棄却
        if (problem.mate_in >= 5) {
          komori::tsume::VerifyOptions short_opts;
          short_opts.max_ply = problem.mate_in - 2;
          short_opts.max_nodes = 300'000;
          short_opts.double_king = false;
          Position short_pos; StateListPtr short_st(new StateList(1));
          short_pos.set(problem.sfen, &short_st->back(), Threads.main());
          komori::tsume::ExhaustiveVerifier short_ev(short_opts);
          const auto short_result = short_ev.Run(short_pos);
          if (short_result.proof == komori::tsume::Proof::kMate) {
            sync_cout << "info string [candidate rejected] shorter mate: "
                      << short_result.mate_ply << "ply (expected " << problem.mate_in << ")"
                      << sync_endl;
            continue;
          }
        }

        // df-pnで見つかった候補を作品検査用の全幅AND/OR探索で再検証する。
        Position candidate; StateListPtr candidate_states(new StateList(1));
        candidate.set(problem.sfen, &candidate_states->back(), Threads.main());
        komori::tsume::ExhaustiveVerifier complete_verifier(verify_options);
        auto complete_result = complete_verifier.Run(candidate);
        // strip_verified_necessary=trueの場合、主変化で駒を使わなくても変化に必要=surplus偽陽性
        const bool surplus = !strip_verified_necessary &&
                             komori::tsume::HasSurplusAttackerHand(candidate, complete_result.principal);
        if (!complete_result.complete || complete_result.proof != komori::tsume::Proof::kMate ||
            !complete_result.unique || complete_result.mate_ply != problem.mate_in || surplus) {
          sync_cout << "info string [candidate rejected] nodes=" << complete_result.nodes
                    << " complete=" << complete_result.complete
                    << " mate=" << (complete_result.proof == komori::tsume::Proof::kMate)
                    << " unique=" << complete_result.unique
                    << " matePly=" << complete_result.mate_ply
                    << " surplus=" << surplus << sync_endl;
          continue;
        }

        AnalyzeUnnecessaryPieces(problem.sfen, verify_options, complete_result);
        if (complete_result.unnecessary_piece) {
          sync_cout << "info string [candidate rejected] unnecessary piece" << sync_endl;
          continue;
        }
        problem.sfen = CanonicalizeVerified(problem.sfen, verify_options, complete_result);

        Position normalized; StateListPtr normalized_states(new StateList(1));
        normalized.set(problem.sfen, &normalized_states->back(), Threads.main());
        komori::tsume::ExhaustiveVerifier normalized_verifier(verify_options);
        complete_result = normalized_verifier.Run(normalized);
        if (!complete_result.complete || !complete_result.unique ||
            complete_result.mate_ply != problem.mate_in) {
          continue;
        }
        problem.solution.clear();
        for (Move m : complete_result.principal) problem.solution.push_back(USI::move(m));
        auto record = komori::tsume::MakeRecord(normalized, problem.sfen, verify_options,
                                                std::move(complete_result));
        if (!record.aesthetic_pass) {
          sync_cout << "info string [candidate rejected] aesthetic gate:"
                    << " perfect=" << record.perfect
                    << " matePly=" << record.verification.mate_ply
                    << " chasing=" << record.techniques.chasing_mate
                    << " score=" << record.scores.aestheticScore
                    << " sacr=" << record.techniques.sacrifices
                    << " non_king_def=" << record.techniques.non_king_defences;
          for (const auto& reason : record.aesthetic_reasons) sync_cout << ' ' << reason << ';';
          sync_cout << sync_endl;
          if (static_cast<int>(found.size()) < target_count)
            try_local_search("aesthetic_reject");
          continue;
        } else {
          problem.record_json = komori::tsume::ToJson(record, normalized);
        }

        // 右上チェック（正規形後）
        komori::detail::SfenBoard cb;
        komori::detail::ParseSfenBoard(problem.sfen.substr(0, problem.sfen.find(' ')), cb);
        bool all_right = true, all_upper = true;
        for (int r = 0; r < 9; ++r)
          for (int f = 0; f < 9; ++f)
            if (!cb[r][f].empty()) {
              if (f > 2) all_right = false;
              if (r > 2) all_upper = false;
            }

        found.push_back(problem);
        sync_cout << "info string [" << problem.mate_in << "手詰め] 発見 "
                  << (static_cast<int>(found.size()) - target_count + count)
                  << "/" << count
                  << (all_right ? " [右寄り]" : "") << (all_upper ? " [上寄り]" : "")
                  << ": " << problem.sfen << sync_endl;
        if (!output_file.empty())
          komori::SaveTsumeProblems({problem}, output_file);
        }  // end if (false)
      }
    }
  }

  if (static_cast<int>(found.size()) < target_count) {
    sync_cout << "info string [" << target_moves
              << "手詰め] 試行回数上限（" << max_attempts << "回）に達しました" << sync_endl;
  }

  // TT とオプションを元に戻す
  Searcher().Init(g_option, static_cast<std::uint32_t>(Threads.size()));
}
}  // namespace

// USI拡張コマンド "user" のディスパッチャ
//
// サブコマンド:
//   user tsume_solve
//       現在の局面を詰将棋ルール検証付きで解答する
//       出力: checkmate <手順> / checkmate nomate / checkmate timeout
//
//   user tsume_generate <target_moves> [count] [output_file]
//       指定手数の詰将棋を count 問生成してファイルに保存する
//       target_moves : 目標手数 (default: 3)
//       count        : 生成問題数 (default: 1)
//       output_file  : 出力ファイル (default: tsume_output.sfen)
//
//   user tsume_batch_generate <start_moves> <end_moves> <step> [count_each] [output_file]
//       start_moves から end_moves まで step 刻みで各手数の詰将棋を生成する
//       count_each   : 各手数の生成問題数 (default: 1)
//       output_file  : 出力ファイル (default: tsume_batch.sfen)
//       例: user tsume_batch_generate 5 31 2 2 batch.sfen
//           → 5,7,9,11,...,31手詰めを各2問生成
// -----------------------------------------------------------------------
// 完全作解答モード（df-pn ベース）
//
// 全幅検証（tsume_verify）は長手数で現実的な時間に終わらないため、KomoringHeights の
// df-pn を部分問題の判定に使う。df-pn 自体は無駄合を有効な応手として数えるので、
// その手数は詰将棋ルールの手数の上界にしかならない。そこで
//   1. 玉方の打ち合（王手している飛び駒と玉の間への打）は、取った後に取った駒を
//      使わずに詰むなら無駄合として応手から外す
//   2. 玉方ノードでは残った応手の詰み手数を上界の大きい順に再帰的に求め、最長を選ぶ
//      （上界が現在の最長以下になった時点で打ち切る）
// として詰将棋ルールでの解を求め、作意（なければ求めた解）上の最終手以外の攻方手番で
// 他の王手が残り手数以内に詰むかを調べて余詰を判定する。
// -----------------------------------------------------------------------
namespace {

struct DfpnOutcome {
  komori::NodeState state{komori::NodeState::kUnknown};
  int len{-1};  // 証明された手数（下界モードでは合駒を2手割り引いた手数）
  std::vector<Move> moves;
  bool min_proven{false};
  std::vector<std::pair<Move, komori::NodeState>> root;  // root_moves 指定時の根の各手の結果
};

/// 手順が合法な詰み手順か。final_check が false なら最終局面の応手の有無は調べない。
bool IsValidMatePath(Position& p, const std::vector<Move>& moves, bool or_root, bool no_drops = false,
                     bool final_check = true) {
  if (moves.size() % 2 != (or_root ? 1u : 0u)) return false;
  std::vector<StateInfo> st(moves.size());
  std::size_t made = 0;
  bool valid = true;
  for (; made < moves.size(); ++made) {
    const Move m = moves[made];
    const bool attacker = (made % 2 == 0) == or_root;
    if (!p.pseudo_legal_s<true>(m) || !p.legal(m) || (attacker && !p.gives_check(m))) { valid = false; break; }
    p.do_move(m, st[made]);
  }
  if (valid && final_check) {
    // 無駄合判定中は、残る応手が王手駒と玉の間への打ち合だけなら詰み（df-pn が無駄合と判定済み）
    const Bitboard checkers = p.checkers();
    Bitboard between{};
    if (checkers.pop_count() == 1) {
      Bitboard tmp = checkers;
      between = between_bb(p.king_square(p.side_to_move()), tmp.pop());
    }
    for (const auto& ext : MoveList<LEGAL_ALL>(p)) {
      const bool futile_drop = komori::g_futile_interposition && is_drop(ext.move) && between.test(to_sq(ext.move));
      if (!(no_drops && is_drop(ext.move)) && !futile_drop) { valid = false; break; }
    }
  }
  while (made-- > 0) p.undo_move(moves[made]);
  return valid;
}

/// df-pn を1回実行する。max_len > 0 ならその手数以内の詰みだけを探す。
DfpnOutcome RunDfpn(const std::string& sfen, std::uint64_t time_ms, int max_len = 0, bool no_drops = false,
                    bool root_moves = false) {
  // no_drops: 下界モード。玉方の打ちを2手少なく数える（無駄合を除いた手数の下界）
  // root_moves: 根の全ての手の詰み／不詰を1回の探索でまとめて調べる（MultiPV）
  komori::g_discount_defender_drops = no_drops;
  const bool saved_futile = komori::g_futile_interposition;
  if (no_drops) komori::g_futile_interposition = false;  // 割引モードでは無駄合判定は不要
  g_option.max_mate_len = max_len;
  const auto saved_multi_pv = g_option.multi_pv;
  const auto saved_post_search = g_option.post_search_level;
  if (root_moves) {
    g_option.multi_pv = 1000;
    g_option.post_search_level = komori::PostSearchLevel::kNone;
  }
  // 合駒あり／なしで結果の意味が違うので置換表を分ける。同じモード同士では置換表を使い回す
  // （証明は手順を並べ直して確認するので、別局面の証明が紛れても誤りにはならない）。
  g_active_searcher = &g_searchers[no_drops ? 1 : 0];
  Searcher().UpdateOption(g_option);
  Position p;
  StateListPtr states(new StateList(1));
  p.set(sfen, &states->back(), Threads.main());
  Search::LimitsType limits;
  limits.mate = static_cast<int>(std::max<std::uint64_t>(1, time_ms));
  Time.reset();
  Threads.start_thinking(p, states, limits, false);
  Threads.main()->wait_for_search_finished();
  DfpnOutcome out;
  out.state = g_search_result;
  if (root_moves) {
    for (const auto& [move, result] : Searcher().RootResults()) {
      out.root.emplace_back(move, result.IsFinal() ? result.GetNodeState() : komori::NodeState::kUnknown);
    }
    g_option.multi_pv = saved_multi_pv;
    g_option.post_search_level = saved_post_search;
  }
  if (out.state == komori::NodeState::kProven && no_drops) {
    // 下界モードでは PV を作らないので手数だけを使う
    out.len = static_cast<int>(Searcher().ProvenLen().Len());
    out.min_proven = Searcher().MinLengthProven();
  } else if (out.state == komori::NodeState::kProven) {
    out.moves = Searcher().BestMoves();
    out.len = static_cast<int>(out.moves.size());
    out.min_proven = Searcher().MinLengthProven();
    // 時間切れ間際に不正な証明が返ることがあるので、手順を並べ直して詰みを確認する
    if (!IsValidMatePath(p, out.moves, IsPosOrNode(p))) {
      out.state = komori::NodeState::kUnknown;
      out.moves.clear();
      out.min_proven = false;
    }
  }
  komori::g_discount_defender_drops = false;
  komori::g_futile_interposition = saved_futile;
  g_active_searcher = &g_searchers[0];
  if (out.state == komori::NodeState::kUnknown) {
    sync_cout << "info string [tsume_answer] unresolved " << sfen << " max_len=" << max_len
              << (no_drops ? " no_drops" : "") << sync_endl;
  }
  return out;
}

bool IsDropInterposition(const Position& pos, Move m) {
  if (!is_drop(m)) return false;
  const Square ksq = pos.king_square(pos.side_to_move());
  Bitboard checkers = pos.checkers();
  while (checkers) {
    if (between_bb(ksq, checkers.pop()).test(to_sq(m))) return true;
  }
  return false;
}

/// SFEN の持ち駒から、指定した手番の駒を1枚取り除く
std::string RemoveOneFromHand(const std::string& sfen, Color c, PieceType pt) {
  std::istringstream in(sfen);
  std::string board, turn, hand, ply;
  in >> board >> turn >> hand >> ply;
  const std::string order = "RBGSNLPrbgsnlp";
  std::array<int, 14> counts{};
  if (hand != "-") {
    int n = 0;
    for (char ch : hand) {
      if (std::isdigit(static_cast<unsigned char>(ch))) { n = n * 10 + (ch - '0'); continue; }
      const auto at = order.find(ch);
      if (at != std::string::npos) counts[at] += n == 0 ? 1 : n;
      n = 0;
    }
  }
  const char upper = "?PLNSBRG"[pt];
  const auto idx = order.find(c == BLACK ? upper : static_cast<char>(std::tolower(upper)));
  if (idx != std::string::npos && counts[idx] > 0) --counts[idx];
  std::string out;
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (counts[i] == 0) continue;
    if (counts[i] > 1) out += std::to_string(counts[i]);
    out += order[i];
  }
  return board + " " + turn + " " + (out.empty() ? "-" : out) + " " + ply;
}

bool HasSurplusAfter(Position& root, const std::vector<Move>& line) {
  std::vector<StateInfo> st(line.size());
  for (std::size_t i = 0; i < line.size(); ++i) root.do_move(line[i], st[i]);
  const bool surplus = root.hand_of(BLACK) != HAND_ZERO;
  for (auto it = line.rbegin(); it != line.rend(); ++it) root.undo_move(*it);
  return surplus;
}

bool g_rule_trace = false;
/// false なら tsume_answer で余詰検証（作意途中の別の王手の探索）を省き、解答だけを求める
bool g_answer_check_alternatives = true;

struct RuleResult {
  int len{-1};            // 上限以内のルール上の詰み手数。-1 なら上限以内に詰まない（不詰を含む）
  bool unknown{false};    // 時間内に判定できなかった
  std::vector<Move> line;
};

/// 無駄合を除いた詰将棋ルールでの手数を、上限付きで求める。
///   - 合駒ありの df-pn 手数はルール上の手数の上界（無駄合も応手として数えるため）
///   - 合駒割引（玉方の打ちを2手少なく数える）の df-pn 手数は下界
/// を使って、攻方ノードでは最短、玉方ノードでは最長を枝刈りしながら求める。
class RuleSolver {
 public:
  explicit RuleSolver(std::uint64_t node_ms) : node_ms_{node_ms} {}

  /// 攻方手番: bound 手以内の最短の詰み
  RuleResult SolveOr(Position& pos, int bound, int depth = 0) {
    if (bound < 1) return {};
    if (depth > 300 || calls_ > kMaxCalls) return Unknown();
    const std::string key = pos.sfen();
    if (auto hit = Lookup(or_memo_, key, bound)) return *hit;
    const int requested = bound;

    const auto& ub = Dfpn(pos.sfen(), bound, false);
    if (ub.state == komori::NodeState::kProven) {
      bound = static_cast<int>(ub.moves.size());
      if (bound == 1) return Store(or_memo_, key, requested, RuleResult{1, false, ub.moves});
    }
    // 候補: 合駒割引の df-pn でも bound 手以内に詰まないと示せなかった王手。
    // 根の全ての手を1回の探索（MultiPV）でまとめて調べる。
    std::vector<std::pair<int, Move>> candidates;  // (並べ替え用の優先度, 王手)
    bool unknown = false;
    if (bound == 1) {
      // 子で0手（応手なし、または全て無駄合）かどうかは SolveAnd で直接調べる。
      // max_len=0 は「上限なし」を意味するので df-pn には渡さない。
      for (const auto& ext : MoveList<LEGAL_ALL>(pos))
        if (pos.gives_check(ext.move)) candidates.emplace_back(0, ext.move);
    } else {
      const auto& lb = DfpnRoot(pos.sfen(), bound);
      for (const auto& [m, state] : lb.root) {
        if (state == komori::NodeState::kDisproven || state == komori::NodeState::kRepetition) continue;
        candidates.emplace_back(state == komori::NodeState::kProven ? 0 : 1, m);
      }
      // 作意の手（上界の最善手）を先に調べると、以降の候補の上限が狭まる
      if (ub.state == komori::NodeState::kProven && !ub.moves.empty()) {
        for (auto& c : candidates) if (c.second == ub.moves.front()) c.first = -1;
      }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    RuleResult r;
    for (const auto& [priority, m] : candidates) {
      const int limit = r.len >= 0 ? r.len - 2 : bound - 1;  // 子で必要な手数上限
      if (limit < 0) break;
      StateInfo st;
      pos.do_move(m, st);
      const auto child = SolveAnd(pos, limit, depth + 1);
      pos.undo_move(m);
      if (child.unknown) { unknown = true; continue; }
      if (child.len < 0) continue;
      r.len = child.len + 1;
      r.line = {m};
      r.line.insert(r.line.end(), child.line.begin(), child.line.end());
    }
    if (unknown) r = Unknown();
    Store(or_memo_, key, requested, r);
    if (g_rule_trace)
      sync_cout << "info string [trace] " << std::string(depth, ' ') << "OR bound=" << bound << " len=" << r.len
                << (r.unknown ? " unknown " : " ") << komori::ToString(r.line) << " | " << pos.sfen() << sync_endl;
    return r;
  }

  /// 玉方手番: 無駄合を除いた最長抵抗。bound 手を超えるなら len = -1
  RuleResult SolveAnd(Position& pos, int bound, int depth = 0) {
    if (depth > 300 || calls_ > kMaxCalls) return Unknown();
    const std::string key = pos.sfen();
    if (auto hit = Lookup(and_memo_, key, bound)) return *hit;

    std::vector<std::pair<int, Move>> candidates;  // (上界, 応手)
    bool any_reply = false;
    for (const auto& ext : MoveList<LEGAL_ALL>(pos)) {
      any_reply = true;
      const Move m = ext.move;
      if (IsDropInterposition(pos, m) && IsFutile(pos, m, bound)) continue;
      // 有効な応手があれば少なくとも2手（応手＋詰めの1手）かかる
      if (bound < 2) return Store(and_memo_, key, bound, RuleResult{});
      StateInfo st;
      pos.do_move(m, st);
      const std::string child = pos.sfen();
      const auto& ub = Dfpn(child, bound - 1, false);
      int upper = bound - 1;
      if (ub.state == komori::NodeState::kProven) {
        upper = static_cast<int>(ub.moves.size());
      } else {
        // 合駒割引でも上限以内に詰まなければ、この応手で上限を超える。
        // 判定できなければ再帰探索（SolveOr）に任せる。
        const auto& lb = Dfpn(child, bound - 1, true);
        if (lb.state == komori::NodeState::kDisproven || lb.state == komori::NodeState::kRepetition) {
          pos.undo_move(m);
          return Store(and_memo_, key, bound, RuleResult{});
        }
      }
      pos.undo_move(m);
      candidates.emplace_back(upper, m);
    }
    if (!any_reply || candidates.empty()) return Store(and_memo_, key, bound, RuleResult{0, false, {}});

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    RuleResult r;
    for (const auto& [upper, m] : candidates) {
      if (r.len >= 0 && upper + 1 < r.len) break;
      StateInfo st;
      pos.do_move(m, st);
      const auto child = SolveOr(pos, std::min(upper, bound - 1), depth + 1);
      pos.undo_move(m);
      if (child.unknown || child.len < 0) {
        RuleResult bad;
        bad.unknown = child.unknown;
        return Store(and_memo_, key, bound, bad);
      }
      const int len = child.len + 1;
      std::vector<Move> cand_line{m};
      cand_line.insert(cand_line.end(), child.line.begin(), child.line.end());
      // 同手数なら駒が余らない手順を選ぶ
      const bool better_tie = len == r.len && HasSurplusAfter(pos, r.line) && !HasSurplusAfter(pos, cand_line);
      if (len > r.len || better_tie) {
        r.len = len;
        r.line = std::move(cand_line);
      }
    }
    if (g_rule_trace) {
      sync_cout << "info string [trace] " << std::string(depth, ' ') << "AND bound=" << bound << " len=" << r.len << " "
                << komori::ToString(r.line) << " cands:";
      for (const auto& [u, m] : candidates) std::cout << ' ' << USI::move(m) << '/' << u;
      std::cout << sync_endl;
    }
    return Store(and_memo_, key, bound, r);
  }

  /// 打ち合 m が無駄合か: 取った後、取った駒を使わずに詰むなら無駄合
  bool IsFutile(Position& pos, Move m, int bound) {
    const Color attacker = ~pos.side_to_move();
    const PieceType dropped = move_dropped_piece(m);
    StateInfo st1;
    pos.do_move(m, st1);
    bool futile = false;
    for (const auto& ext : MoveList<LEGAL_ALL>(pos)) {
      const Move c = ext.move;
      if (is_drop(c) || to_sq(c) != to_sq(m) || !pos.gives_check(c)) continue;
      StateInfo st2;
      pos.do_move(c, st2);
      const std::string sfen = RemoveOneFromHand(pos.sfen(), attacker, dropped);
      pos.undo_move(c);
      if (Dfpn(sfen, std::max(bound, 1), false).state == komori::NodeState::kProven) { futile = true; break; }
    }
    pos.undo_move(m);
    return futile;
  }

  /// 合駒割引・手数上限付きで、根（攻方手番）の全ての王手の詰み／不詰をまとめて調べる
  const DfpnOutcome& DfpnRoot(const std::string& sfen, int max_len) {
    const std::string key = sfen + "|" + std::to_string(max_len) + "|root";
    if (auto it = dfpn_memo_.find(key); it != dfpn_memo_.end()) return it->second;
    ++calls_;
    return dfpn_memo_[key] = RunDfpn(sfen, node_ms_ * 2, max_len, true, true);
  }

  const DfpnOutcome& Dfpn(const std::string& sfen, int max_len, bool no_drops) {
    const std::string key = sfen + "|" + std::to_string(max_len) + (no_drops ? "|nd" : "");
    if (auto it = dfpn_memo_.find(key); it != dfpn_memo_.end()) return it->second;
    ++calls_;
    auto out = RunDfpn(sfen, node_ms_, max_len, no_drops);
    // 時間切れのときだけ、時間を増やして1回だけ再試行する。
    // 合駒割引（下界）は枝刈り用で、決着しなくても再帰探索で判定できるので再試行しない。
    if (out.state == komori::NodeState::kUnknown && !no_drops) out = RunDfpn(sfen, node_ms_ * 2, max_len, no_drops);
    return dfpn_memo_[key] = out;
  }

  std::uint64_t Calls() const { return calls_; }

 private:
  static RuleResult Unknown() { RuleResult r; r.unknown = true; return r; }
  static constexpr std::uint64_t kMaxCalls = 50000;

  std::uint64_t node_ms_;
  std::uint64_t calls_{0};
  std::unordered_map<std::string, DfpnOutcome> dfpn_memo_;
  /// 局面ごとのメモ。exact は上限内で求まった真の手数、exceeded は「この手数を超える」ことが分かった上限。
  struct Memo {
    bool has_exact{false};
    RuleResult exact;
    int exceeded{-1};
    int unknown_at{-1};
  };

  static std::optional<RuleResult> Lookup(std::unordered_map<std::string, Memo>& memo, const std::string& key,
                                          int bound) {
    const auto it = memo.find(key);
    if (it == memo.end()) return std::nullopt;
    const auto& m = it->second;
    if (m.has_exact) return m.exact.len <= bound ? m.exact : RuleResult{};
    if (m.exceeded >= bound) return RuleResult{};
    if (m.unknown_at == bound) return Unknown();
    return std::nullopt;
  }

  static RuleResult Store(std::unordered_map<std::string, Memo>& memo, const std::string& key, int bound,
                          const RuleResult& r) {
    auto& m = memo[key];
    if (r.unknown) m.unknown_at = bound;
    else if (r.len >= 0) { m.has_exact = true; m.exact = r; }
    else m.exceeded = std::max(m.exceeded, bound);
    return r;
  }

  std::unordered_map<std::string, Memo> or_memo_, and_memo_;
};

const char* NodeStateName(komori::NodeState s) {
  switch (s) {
    case komori::NodeState::kProven: return "mate";
    case komori::NodeState::kDisproven: return "nomate";
    case komori::NodeState::kRepetition: return "repetition";
    default: return "unknown";
  }
}

/// 詰将棋ルールで組み立てた解答手順
struct AnswerLine {
  std::vector<Move> moves;
  bool surplus{false};  ///< 詰め上がりで攻方の持駒が余る
  bool ok{false};       ///< 手順を組み立てられた
};

/// 変同（同手数・駒余りなしの応手が複数ある玉方手番）
struct Hendou {
  int ply;                       ///< 手目（1 始まり）
  std::vector<Move> prefix;      ///< その手番までの手順
  std::vector<AnswerLine> lines; ///< 同手数の各応手からの手順
};

//   user tsume_answer <部分問題1つあたりms> <全体のdf-pn ms> [作意USI手順...]
void AnswerTsume(Position& pos, std::istringstream& is) {
  std::uint64_t node_ms = 3000, raw_ms = 30000;
  std::string arg;
  if (is >> arg) node_ms = std::stoull(arg);
  if (is >> arg) raw_ms = std::stoull(arg);
  std::vector<std::string> intended_usi;
  while (is >> arg) intended_usi.push_back(arg);

  const bool double_king = pos.king_square(pos.side_to_move()) != SQ_NB;
  const std::string sfen = komori::detail::CompleteDefenderReserve(pos.sfen(), double_king);
  std::ostringstream json;
  json << "{\"sfen\":\"" << sfen << "\"";

  Position root;
  StateListPtr root_states(new StateList(1));
  root.set(sfen, &root_states->back(), Threads.main());
  if (const auto err = komori::ValidateTsumePosition(root); !err.empty()) {
    sync_cout << "info string tsume_answer " << json.str() << ",\"status\":\"invalid\",\"error\":\""
              << komori::tsume::detail::EscapeJson(err) << "\"}" << sync_endl;
    return;
  }

  const auto saved_option = g_option;
  const bool saved_silent = g_tsume_gen_silent;
  komori::EngineOption quiet = g_option;
  quiet.silent = true;
  quiet.pv_interval = std::numeric_limits<std::uint64_t>::max() / 2;
  quiet.post_search_level = komori::PostSearchLevel::kMinLength;
  quiet.root_is_and_node_if_checked = true;
  g_option = quiet;
  for (auto& searcher : g_searchers) searcher.Init(quiet, static_cast<std::uint32_t>(Threads.size()));
  g_tsume_gen_silent = true;

  const auto start = std::chrono::steady_clock::now();
  // 無駄合を応手として数えない df-pn で解く（詰将棋ルールでの最短手数・最長抵抗）
  komori::g_futile_interposition = true;
  std::uint64_t dfpn_calls = 0;
  auto dfpn = [&](const std::string& s, std::uint64_t ms, int max_len) {
    ++dfpn_calls;
    auto out = RunDfpn(s, ms, max_len);
    if (out.state == komori::NodeState::kUnknown) out = RunDfpn(s, ms * 4, max_len);
    return out;
  };

  // 上限なしの探索は無駄合判定用の局面 V にも上限がなくなって遅い。そこでまず無駄合判定なしの df-pn で
  // 手数の上界（無駄合も応手に数えた手数）を求め、その上限で無駄合判定ありの探索をする。
  // 最短性（それより短い詰みがないこと）は MinLength の余詰探索で残り時間の範囲で証明する。
  DfpnOutcome solved;
  bool uncertain = false;
  {
    komori::g_futile_interposition = false;
    const auto upper = RunDfpn(sfen, raw_ms / 4, 0);
    ++dfpn_calls;
    // 無駄合判定なしの結果（無駄合を応手に数えた不詰など）が混ざらないよう置換表を消す
    g_searchers[0].Clear();
    komori::g_futile_interposition = true;
    const int bound = upper.state == komori::NodeState::kProven ? static_cast<int>(upper.moves.size()) : 0;
    komori::g_futile_probe_len = bound > 0 ? bound : 31;  // 解答の組み立て直しの上限に使う
    solved = dfpn(sfen, raw_ms, bound);
    uncertain = solved.state == komori::NodeState::kProven && !solved.min_proven;
  }
  if (uncertain && solved.state == komori::NodeState::kProven) json << ",\"minLengthUncertain\":true";
  const int mate_ply = solved.state == komori::NodeState::kProven ? static_cast<int>(solved.moves.size()) : -1;
  json << ",\"status\":\"" << NodeStateName(solved.state) << "\",\"matePly\":" << mate_ply
       << ",\"minLengthProven\":" << (solved.min_proven ? "true" : "false")
       << ",\"usi\":\"" << komori::ToString(solved.moves) << "\"";

  // 玉方は最長手順を選び、同手数なら攻方の持駒が余らない手順を選ぶ（詰将棋ルール）。
  // df-pn の手順はこのタイブレークをしないので、玉方手番ごとに最長の応手を全て調べ直して組み立てる。
  std::vector<Hendou> hendou;
  // 打ち合 m（玉方手番の pos で）が無駄合か: 取って駒を返した局面が詰むか
  auto is_futile_drop = [&](Position& p, Move m) {
    if (!is_drop(m)) return false;
    const Bitboard checkers = p.checkers();
    if (checkers.pop_count() != 1) return false;
    Bitboard tmp = checkers;
    if (!between_bb(p.king_square(p.side_to_move()), tmp.pop()).test(to_sq(m))) return false;
    StateInfo s1;
    p.do_move(m, s1);
    bool futile = false;
    for (const auto& ext : MoveList<LEGAL_ALL>(p)) {
      const Move c = ext.move;
      if (is_drop(c) || to_sq(c) != to_sq(m) || !p.gives_check(c)) continue;
      StateInfo s2;
      p.do_move(c, s2);
      const PieceType pr = raw_type_of(p.state()->capturedPiece);
      p.steal_hand(pr);
      const std::string v = p.sfen();
      p.give_hand(pr);
      p.undo_move(c);
      if (dfpn(v, node_ms, komori::g_futile_probe_len).state == komori::NodeState::kProven) { futile = true; break; }
    }
    p.undo_move(m);
    return futile;
  };
  std::function<AnswerLine(Position&, int, std::vector<Move>&)> build_line =
      [&](Position& p, int remaining, std::vector<Move>& prefix) -> AnswerLine {
    AnswerLine out;
    if (remaining <= 0) {
      out.ok = true;
      out.surplus = p.hand_of(BLACK) != HAND_ZERO;
      return out;
    }
    const bool or_node = remaining % 2 == 1;
    if (or_node && remaining == 1) {
      // 攻方の最終手: 詰ませる手を全て調べる。駒を余らせて詰む手が1つでもあれば、この変化は駒余り
      // （玉方は攻方に持駒を全部使わせるように逃げなければならない）。解答には駒余りのない詰め手を使う。
      std::vector<Move> clean_mates, surplus_mates;
      for (const auto& ext : MoveList<LEGAL_ALL>(p)) {
        const Move m = ext.move;
        if (!p.gives_check(m)) continue;
        StateInfo st;
        p.do_move(m, st);
        const bool mate = dfpn(p.sfen(), node_ms, 1).state == komori::NodeState::kProven;
        const bool surplus = p.hand_of(BLACK) != HAND_ZERO;
        p.undo_move(m);
        if (mate) (surplus ? surplus_mates : clean_mates).push_back(m);
      }
      if (clean_mates.empty() && surplus_mates.empty()) return out;
      out.ok = true;
      out.surplus = !surplus_mates.empty();
      out.moves = {clean_mates.empty() ? surplus_mates.front() : clean_mates.front()};
      return out;
    }
    if (or_node) {
      const auto d = dfpn(p.sfen(), node_ms, remaining);
      if (d.state != komori::NodeState::kProven || d.moves.empty()) return out;
      const Move m = d.moves.front();
      StateInfo st;
      p.do_move(m, st);
      prefix.push_back(m);
      auto child = build_line(p, remaining - 1, prefix);
      prefix.pop_back();
      p.undo_move(m);
      if (!child.ok) return out;
      out = child;
      out.moves.insert(out.moves.begin(), m);
      return out;
    }
    // 玉方手番: 無駄合を除く各応手の正確な手数を求め、最長のものを残す
    std::vector<Move> longest;
    for (const auto& ext : MoveList<LEGAL_ALL>(p)) {
      const Move m = ext.move;
      if (is_futile_drop(p, m)) continue;
      StateInfo st;
      p.do_move(m, st);
      const auto d = dfpn(p.sfen(), node_ms, remaining - 1);
      p.undo_move(m);
      if (d.state == komori::NodeState::kProven && static_cast<int>(d.moves.size()) == remaining - 1) {
        longest.push_back(m);
      }
    }
    // 同手数・駒余りなしで並んだときは、攻方の駒を取る手（大駒ほど優先）、次に合駒でない手を選ぶ
    auto reply_rank = [&](Move m) {
      int rank = 0;
      if (!is_drop(m) && p.piece_on(to_sq(m)) != NO_PIECE) {
        static constexpr int kValue[] = {0, 1, 2, 3, 4, 6, 7, 5, 0};  // -, 歩, 香, 桂, 銀, 角, 飛, 金, 玉
        const PieceType pt = raw_type_of(p.piece_on(to_sq(m)));
        rank += 100 + kValue[pt] * 10 + (type_of(p.piece_on(to_sq(m))) != pt ? 5 : 0);
      }
      if (!is_drop(m)) rank += 1;
      return rank;
    };
    std::stable_sort(longest.begin(), longest.end(), [&](Move a, Move b) { return reply_rank(a) > reply_rank(b); });
    std::vector<AnswerLine> lines;
    for (const Move m : longest) {
      StateInfo st;
      p.do_move(m, st);
      prefix.push_back(m);
      auto child = build_line(p, remaining - 1, prefix);
      prefix.pop_back();
      p.undo_move(m);
      if (!child.ok) continue;
      child.moves.insert(child.moves.begin(), m);
      lines.push_back(std::move(child));
    }
    if (lines.empty()) return out;
    // 駒余りのない手順を優先する
    std::stable_partition(lines.begin(), lines.end(), [](const AnswerLine& l) { return !l.surplus; });
    const auto clean = std::count_if(lines.begin(), lines.end(), [](const AnswerLine& l) { return !l.surplus; });
    // 玉方最終手（どの応手にも1手で詰む）の違いは変同として数えない
    if (clean >= 2 && remaining > 2) {
      std::vector<AnswerLine> same(lines.begin(), lines.begin() + clean);
      hendou.push_back(Hendou{static_cast<int>(prefix.size()) + 1, prefix, std::move(same)});
    }
    return lines.front();
  };

  // 解答手順を組み立てる。組み立てられなければ df-pn の手数は誤り（無駄合判定が手数上限に依存するため、
  // まれに短すぎる手数が返る）なので、2手ずつ長くして組み立て直す。
  AnswerLine answer;
  int answer_ply = mate_ply;
  if (solved.state == komori::NodeState::kProven) {
    for (; answer_ply <= std::max(mate_ply, komori::g_futile_probe_len) + 4; answer_ply += 2) {
      std::vector<Move> prefix;
      hendou.clear();
      answer = build_line(root, answer_ply, prefix);
      if (answer.ok) break;
    }
    if (answer.ok && answer_ply != mate_ply) json << ",\"lengthCorrectedFrom\":" << mate_ply;
  }
  if (answer.ok) {
    json << ",\"answerPly\":" << answer.moves.size();
    json << ",\"answer\":\"" << komori::ToString(answer.moves) << "\",\"answerSurplus\":"
         << (answer.surplus ? "true" : "false");
  }
  json << ",\"hendou\":[";
  for (std::size_t i = 0; i < hendou.size(); ++i) {
    if (i) json << ',';
    json << "{\"ply\":" << hendou[i].ply << ",\"prefix\":\"" << komori::ToString(hendou[i].prefix)
         << "\",\"lines\":[";
    for (std::size_t j = 0; j < hendou[i].lines.size(); ++j) {
      if (j) json << ',';
      json << '"' << komori::ToString(hendou[i].lines[j].moves) << '"';
    }
    json << "]}";
  }
  json << "]";

  // 作意の検査
  std::vector<Move> intended;
  bool intended_legal = !intended_usi.empty();
  {
    std::vector<StateInfo> st(intended_usi.size());
    for (std::size_t i = 0; i < intended_usi.size(); ++i) {
      const Move m = USI::to_move(root, intended_usi[i]);
      if (m == MOVE_NONE) { intended_legal = false; break; }
      intended.push_back(m);
      root.do_move(m, st[i]);
    }
    std::string final_sfen = root.sfen();
    for (auto it = intended.rbegin(); it != intended.rend(); ++it) root.undo_move(*it);
    if (intended_legal) intended_legal = IsValidMatePath(root, intended, true, false, false);
    if (intended_legal) {
      // 最終局面は、応手が無いか全て無駄合であること（玉方手番で 0 手詰）
      intended_legal = dfpn(final_sfen, node_ms, 1).state == komori::NodeState::kProven;
    }
    if (!intended_legal) intended.clear();
  }
  if (!intended_usi.empty()) {
    json << ",\"intendedPly\":" << intended_usi.size()
         << ",\"intendedLegal\":" << (intended_legal ? "true" : "false")
         << ",\"sameAsIntended\":" << (intended_legal && answer.moves == intended ? "true" : "false");
  }

  // 余詰を調べる手順: 作意が正しく駒余りもなければ作意、それ以外は組み立てた解答
  const int final_ply = answer.ok ? static_cast<int>(answer.moves.size()) : mate_ply;
  const bool use_intended = intended_legal && static_cast<int>(intended.size()) == final_ply &&
                            !HasSurplusAfter(root, intended);
  const std::vector<Move>& line = use_intended ? intended : (answer.ok ? answer.moves : solved.moves);
  int alternatives = 0, unresolved = 0;
  json << ",\"checkedLine\":\"" << (use_intended ? "intended" : "answer") << "\",\"alternatives\":[";
  bool first = true;
  if (!line.empty()) {
    std::vector<StateInfo> st(line.size());
    for (std::size_t i = 0; i < line.size(); ++i) {
      if (g_answer_check_alternatives && i % 2 == 0 && i + 1 < line.size()) {
        const int remaining = static_cast<int>(line.size() - i);
        for (const auto& ext : MoveList<LEGAL_ALL>(root)) {
          const Move m = ext.move;
          if (m == line[i] || !root.gives_check(m)) continue;
          StateInfo alt_st;
          root.do_move(m, alt_st);
          // 残り手数以内に詰むか（無駄合は数えない）。まず合駒割引の軽い探索で否定を試みる
          const std::string alt_sfen = root.sfen();
          const auto lower = RunDfpn(alt_sfen, node_ms, remaining - 1, true);
          ++dfpn_calls;
          DfpnOutcome d;
          if (lower.state == komori::NodeState::kDisproven || lower.state == komori::NodeState::kRepetition) {
            d = lower;
          } else {
            d = dfpn(alt_sfen, node_ms, remaining - 1);
          }
          root.undo_move(m);
          if (d.state == komori::NodeState::kDisproven || d.state == komori::NodeState::kRepetition) continue;
          std::string kind;
          int alt_len = -1;
          // 作意と同じ升への成・不成違いは余詰ではなく非限定として扱う
          const bool promotion_variant = !is_drop(m) && !is_drop(line[i]) && from_sq(m) == from_sq(line[i]) &&
                                         to_sq(m) == to_sq(line[i]);
          if (d.state != komori::NodeState::kProven) { kind = "unknown"; ++unresolved; }
          else {
            alt_len = static_cast<int>(d.moves.size()) + 1;
            if (promotion_variant) kind = "promotion-nonlimited";
            else { kind = alt_len < remaining ? "shorter" : "alternative"; ++alternatives; }
          }
          if (!first) json << ',';
          first = false;
          std::vector<Move> alt_line{m};
          alt_line.insert(alt_line.end(), d.moves.begin(), d.moves.end());
          json << "{\"ply\":" << i + 1 << ",\"move\":\"" << USI::move(m) << "\",\"kind\":\"" << kind
               << "\",\"matePly\":" << alt_len << ",\"line\":\"" << komori::ToString(alt_line) << "\"}";
        }
      }
      root.do_move(line[i], st[i]);
    }
    for (auto it = line.rbegin(); it != line.rend(); ++it) root.undo_move(*it);
  }
  json << "]";
  komori::g_futile_interposition = false;

  const bool surplus = !line.empty() && HasSurplusAfter(root, line);
  const bool length_ok = final_ply >= 0 && solved.min_proven && answer.ok && final_ply == mate_ply &&
                         (intended_usi.empty() || (intended_legal && final_ply == static_cast<int>(intended.size())));
  const bool perfect = g_answer_check_alternatives && length_ok && alternatives == 0 && unresolved == 0 && !surplus;
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start).count();
  json << ",\"alternativeCount\":" << alternatives << ",\"unresolvedCount\":" << unresolved
       << ",\"pieceSurplus\":" << (surplus ? "true" : "false")
       << ",\"lengthMatchesIntended\":" << (length_ok ? "true" : "false")
       << ",\"alternativesChecked\":" << (g_answer_check_alternatives ? "true" : "false")
       << ",\"perfect\":" << (perfect ? "true" : "false") << ",\"dfpnCalls\":" << dfpn_calls
       << ",\"timeMs\":" << elapsed << "}";

  g_tsume_gen_silent = saved_silent;
  g_option = saved_option;
  // 無駄合ルールで書いた置換表を通常の探索で使わないよう作り直す
  g_searchers[0].Init(saved_option, static_cast<std::uint32_t>(Threads.size()));
  sync_cout << "info string tsume_answer " << json.str() << sync_endl;
}

}  // namespace

void user_test(Position& pos, std::istringstream& is) {
  std::string token;
  is >> token;

  auto verify_and_output = [&](const std::string& input_sfen, komori::tsume::VerifyOptions options,
                               const std::string& save_file) {
    Position verify_pos; StateListPtr verify_states(new StateList(1));
    verify_pos.set(input_sfen, &verify_states->back(), Threads.main());
    const std::string canonical_input_sfen = verify_pos.sfen();
    const auto err = komori::ValidateTsumePosition(verify_pos);
    if (!err.empty()) { sync_cout << "info string [tsume] position error: " << err << sync_endl; return; }
    komori::tsume::ExhaustiveVerifier verifier(options);
    auto result = verifier.Run(verify_pos);
    const std::string normalized = result.complete
        ? CanonicalizeVerified(canonical_input_sfen, options, result) : canonical_input_sfen;
    AnalyzeUnnecessaryPieces(canonical_input_sfen, options, result);
    auto record = komori::tsume::MakeRecord(verify_pos, normalized, options, std::move(result));
    sync_cout << "info string tsume_json " << komori::tsume::ToJson(record, verify_pos) << sync_endl;
    if (!save_file.empty())
      sync_cout << "info string [tsume_save] "
                << (komori::tsume::SaveJson(record, verify_pos, save_file) ? "saved " : "save failed ")
                << save_file << sync_endl;
    if (record.verification.proof == komori::tsume::Proof::kMate)
      sync_cout << "checkmate " << komori::tsume::detail::UsiLine(record.verification.principal) << sync_endl;
    else if (record.verification.complete) sync_cout << "checkmate nomate" << sync_endl;
    else sync_cout << "checkmate timeout" << sync_endl;
  };

  // user tsume_load <jsonl> [max_ply] [max_nodes]
  if (token == "tsume_load") {
    std::string filename; is >> filename;
    std::string loaded_sfen;
    if (filename.empty() || !komori::tsume::LoadSfenFromJson(filename, loaded_sfen)) {
      sync_cout << "info string [tsume_load] load failed " << filename << sync_endl; return;
    }
    komori::tsume::VerifyOptions options; std::string arg;
    if (is >> arg) options.max_ply = std::max(1, std::stoi(arg));
    if (is >> arg) options.max_nodes = std::max<std::uint64_t>(1, std::stoull(arg));
    Position probe; StateListPtr states(new StateList(1)); probe.set(loaded_sfen, &states->back(), Threads.main());
    options.double_king = probe.king_square(probe.side_to_move()) != SQ_NB;
    verify_and_output(komori::detail::CompleteDefenderReserve(loaded_sfen, options.double_king), options, "");
    return;
  }

  // Local extension search from the current composition.
  // user tsume_extend <count> <max_ply> <max_nodes> <jsonl>
  if (token == "tsume_extend") {
    int wanted_count = 3; komori::tsume::VerifyOptions options; std::string arg, filename = "tsume_extensions.jsonl";
    if (is >> arg) wanted_count = std::max(1, std::stoi(arg));
    if (is >> arg) options.max_ply = std::max(7, std::stoi(arg));
    if (is >> arg) options.max_nodes = std::max<std::uint64_t>(1, std::stoull(arg));
    if (is >> arg) filename = arg;
    options.double_king = pos.king_square(pos.side_to_move()) != SQ_NB;
    const std::string base_sparse = pos.sfen();
    const std::string base_sfen = komori::detail::CompleteDefenderReserve(base_sparse, options.double_king);
    Position base; StateListPtr base_states(new StateList(1)); base.set(base_sfen, &base_states->back(), Threads.main());
    komori::tsume::ExhaustiveVerifier base_verifier(options); auto base_result = base_verifier.Run(base);
    if (!base_result.complete || base_result.proof != komori::tsume::Proof::kMate) {
      sync_cout << "info string [tsume_extend] base position is not a verified mate" << sync_endl; return;
    }
    const std::string parent_id = komori::tsume::detail::StableId(base.sfen() + komori::tsume::detail::UsiLine(base_result.principal));
    std::istringstream parts(base_sparse); std::string board_text, turn, hands, move_no;
    parts >> board_text >> turn >> hands >> move_no;
    komori::detail::SfenBoard source; komori::detail::ParseSfenBoard(board_text, source);
    std::mt19937 rng(std::random_device{}()); std::uniform_int_distribution<int> sq(0, 8), type(0, 6);
    const std::array<char, 7> piece_chars{{'P','L','N','S','G','B','R'}};
    std::unordered_set<std::string> ids; int saved = 0;
    for (int attempt = 0; attempt < 1000 && saved < wanted_count; ++attempt) {
      auto board = source; const int operation = attempt % 6; std::string diff;
      std::vector<std::pair<int,int>> movable, empty;
      for (int r = 0; r < 9; ++r) for (int f = 0; f < 9; ++f) {
        if (board[r][f].empty()) empty.push_back({r,f});
        else if (board[r][f] != "k" && board[r][f] != "K") movable.push_back({r,f});
      }
      if (operation == 0 && !movable.empty() && !empty.empty()) {
        auto from = movable[rng() % movable.size()], to = empty[rng() % empty.size()];
        board[to.first][to.second] = board[from.first][from.second]; board[from.first][from.second].clear(); diff = "piece move";
      } else if (operation == 1 && !movable.empty()) {
        auto at = movable[rng() % movable.size()]; board[at.first][at.second].clear(); diff = "piece removal";
      } else if (operation == 2 && !movable.empty()) {
        auto at = movable[rng() % movable.size()]; const bool black = std::isupper(static_cast<unsigned char>(board[at.first][at.second].back()));
        char c = piece_chars[type(rng)]; board[at.first][at.second] = std::string(1, black ? c : static_cast<char>(std::tolower(c))); diff = "piece type change";
      } else if (operation == 3 && !movable.empty()) {
        auto at = movable[rng() % movable.size()]; auto& p = board[at.first][at.second];
        const char raw = static_cast<char>(std::toupper(static_cast<unsigned char>(p.back())));
        if (raw != 'G') { p = p.size() == 2 ? p.substr(1) : "+" + p; diff = "promotion toggle"; }
      } else if (operation == 4 && !empty.empty()) {
        bool done = false; for (int r = 0; r < 9 && !done; ++r) for (int f = 0; f < 9 && !done; ++f)
          if (board[r][f] == "k") { auto to = empty[rng() % empty.size()]; board[to.first][to.second] = "k"; board[r][f].clear(); done = true; diff = "defender king move"; }
      } else if (operation == 5 && !empty.empty()) {
        auto to = empty[rng() % empty.size()]; board[to.first][to.second] = std::string(1, piece_chars[type(rng)]); diff = "attacker piece addition";
      }
      if (diff.empty()) continue;
      std::string candidate_sparse = komori::detail::BuildSfenBoard(board) + " " + turn + " " + hands + " " + move_no;
      std::string candidate_sfen = komori::detail::CompleteDefenderReserve(candidate_sparse, options.double_king);
      Position candidate; StateListPtr states(new StateList(1)); candidate.set(candidate_sfen, &states->back(), Threads.main());
      if (!komori::ValidateTsumePosition(candidate).empty()) continue;
      komori::tsume::ExhaustiveVerifier verifier(options); auto result = verifier.Run(candidate);
      if (!result.complete || result.proof != komori::tsume::Proof::kMate || !result.unique) continue;
      AnalyzeUnnecessaryPieces(candidate.sfen(), options, result);
      auto normalized = CanonicalizeVerified(candidate.sfen(), options, result);
      auto record = komori::tsume::MakeRecord(candidate, normalized, options, std::move(result), parent_id);
      if (!record.aesthetic_pass || !ids.insert(record.canonical_id).second) continue;
      int same = 0, total = 0; for (int r = 0; r < 9; ++r) for (int f = 0; f < 9; ++f)
        if (!source[r][f].empty() || !board[r][f].empty()) { ++total; if (source[r][f] == board[r][f]) ++same; }
      record.parent_similarity = total ? double(same) / total : 1.0; record.parent_mate_ply = base_result.mate_ply;
      record.parent_diff = diff; record.extension_reason = record.verification.mate_ply > base_result.mate_ply
          ? "added a forced checking/escape phase" : "preserved the mating structure under local mutation";
      if (!komori::tsume::SaveJson(record, candidate, filename)) break;
      ++saved; sync_cout << "info string [tsume_extend] " << saved << '/' << wanted_count << ' '
                        << base_result.mate_ply << "->" << record.verification.mate_ply << " " << diff << sync_endl;
    }
    sync_cout << "info string [tsume_extend] saved " << saved << " candidates to " << filename << sync_endl;
    return;
  }

  // Bounded exhaustive composition verification.  Unlike tsume_solve this
  // enumerates every root attack and every legal defence and emits one JSON
  // object suitable for saving/reloading by tools.
  //   user tsume_verify [max_ply] [max_nodes] [known USI moves...]
  if (token == "tsume_answer_trace") {
    std::string v;
    is >> v;
    g_rule_trace = v == "on";
    return;
  }
  // デバッグ用: 現局面を無駄合ルールで解く
  //   user tsume_futile_mate <ms> <max_len>
  if (token == "tsume_futile_mate") {
    std::uint64_t ms = 3000;
    int max_len = 0;
    is >> ms >> max_len;
    const auto saved = g_option;
    g_option.silent = true;
    g_option.root_is_and_node_if_checked = true;
    g_option.post_search_level = komori::PostSearchLevel::kMinLength;
    g_searchers[0].Init(g_option, static_cast<std::uint32_t>(Threads.size()));
    g_tsume_gen_silent = true;
    komori::g_futile_interposition = true;
    const auto out = RunDfpn(pos.sfen(), ms, max_len);
    komori::g_futile_interposition = false;
    g_tsume_gen_silent = false;
    sync_cout << "info string futile_mate " << NodeStateName(out.state) << " len=" << out.len << " pv=" << komori::ToString(out.moves)
              << sync_endl;
    g_option = saved;
    g_searchers[0].Init(saved, static_cast<std::uint32_t>(Threads.size()));
    return;
  }
  if (token == "tsume_answer_alternatives") {
    std::string v;
    is >> v;
    g_answer_check_alternatives = v != "off";
    return;
  }
  if (token == "tsume_answer") {
    AnswerTsume(pos, is);
    return;
  }

  if (token == "tsume_verify") {
    const bool double_king = pos.king_square(pos.side_to_move()) != SQ_NB;
    const std::string completed_sfen =
        komori::detail::CompleteDefenderReserve(pos.sfen(), double_king);
    komori::tsume::VerifyOptions options;
    std::string arg;
    if (is >> arg) options.max_ply = std::max(1, std::stoi(arg));
    if (is >> arg) options.max_nodes = std::max<std::uint64_t>(1, std::stoull(arg));
    while (is >> arg) options.intended_usi.push_back(arg);
    options.double_king = double_king;
    verify_and_output(completed_sfen, options, "");
    return;
  }

  // user tsume_save <max_ply> <max_nodes> <jsonl> [known USI moves...]
  if (token == "tsume_save") {
    komori::tsume::VerifyOptions options; std::string arg, filename;
    if (is >> arg) options.max_ply = std::max(1, std::stoi(arg));
    if (is >> arg) options.max_nodes = std::max<std::uint64_t>(1, std::stoull(arg));
    is >> filename; while (is >> arg) options.intended_usi.push_back(arg);
    options.double_king = pos.king_square(pos.side_to_move()) != SQ_NB;
    verify_and_output(komori::detail::CompleteDefenderReserve(pos.sfen(), options.double_king), options, filename);
    return;
  }

  // -----------------------------------------------------------------------
  // モード1: 詰将棋解答モード
  // -----------------------------------------------------------------------
  if (token == "tsume_solve") {
    const auto err = komori::ValidateTsumePosition(pos);
    if (!err.empty()) {
      sync_cout << "info string [tsume_solve] 局面エラー: " << err << sync_endl;
      return;
    }

    sync_cout << "info string [tsume_solve] ルール検証: OK" << sync_endl;
    sync_cout << "info string [tsume_solve] 探索開始..." << sync_endl;

    const std::string sfen = pos.sfen();
    Position solve_pos;
    StateListPtr st(new StateList(1));
    solve_pos.set(sfen, &st->back(), Threads.main());

    Search::LimitsType limits;
    limits.mate = INT32_MAX;

    Time.reset();
    Threads.start_thinking(solve_pos, st, limits, false);
    Threads.main()->wait_for_search_finished();

    if (g_search_result == komori::NodeState::kProven) {
      sync_cout << "info string [tsume_solve] "
                << Searcher().BestMoves().size() << "手詰め" << sync_endl;
    } else if (g_search_result == komori::NodeState::kDisproven) {
      sync_cout << "info string [tsume_solve] 不詰" << sync_endl;
    } else if (g_search_result == komori::NodeState::kRepetition) {
      sync_cout << "info string [tsume_solve] 連続王手の千日手（攻め方の負け）" << sync_endl;
    } else {
      sync_cout << "info string [tsume_solve] 探索打ち切り" << sync_endl;
    }
    return;
  }

  // -----------------------------------------------------------------------
  // モード2: 詰将棋単手数生成モード
  // user tsume_generate <target_moves> [count] [output_file]
  // -----------------------------------------------------------------------
  if (token == "tsume_generate") {
    int target_moves        = 9;
    int count               = 1;
    std::string output_file = "tsume_output.sfen";
    {
      std::string t;
      if (is >> t) target_moves = std::stoi(t);
      if (is >> t) count        = std::stoi(t);
      if (is >> t) output_file  = t;
    }
    if (target_moves < 7 || target_moves % 2 == 0) {
      sync_cout << "info string [tsume_generate] エラー: 正式生成は7手以上の奇数のみです" << sync_endl;
      return;
    }

    sync_cout << "info string [tsume_generate] 開始: "
              << target_moves << "手詰め × " << count << "問"
              << " → " << output_file << sync_endl;
    sync_cout << "info string [tsume_generate] 時間上限: "
              << komori::detail::ComputeTimeLimitMs(target_moves) << "ms/局面" << sync_endl;

    std::mt19937 rng(std::random_device{}());
    // 既存ファイルを読み込み、重複・鏡像スキップに活用（target = count 問合計）
    auto found = komori::LoadTsumeProblems(output_file);
    const int existing = static_cast<int>(found.size());
    if (existing > 0)
      sync_cout << "info string [tsume_generate] 既存問題ロード: " << existing << "問" << sync_endl;
    const int need = std::max(0, count - existing);
    if (need == 0) {
      sync_cout << "info string [tsume_generate] 既に" << existing << "問あります（目標達成）" << sync_endl;
      return;
    }

    g_tsume_gen_silent = true;
    GenerateProblemsForMoves(target_moves, need, rng, found, output_file);
    g_tsume_gen_silent = false;

    if (found.empty()) {
      sync_cout << "info string [tsume_generate] 問題を生成できませんでした" << sync_endl;
    } else if (output_file.empty()) {
      komori::SaveTsumeProblems(found, output_file);
    }
    return;
  }

  // -----------------------------------------------------------------------
  // モード3: 詰将棋バッチ生成モード
  // user tsume_batch_generate <start_moves> <end_moves> <step> [count_each] [output_file]
  // -----------------------------------------------------------------------
  if (token == "tsume_batch_generate") {
    int start_moves         = 7;
    int end_moves           = 11;
    int step                = 2;
    int count_each          = 1;
    std::string output_file = "tsume_batch.sfen";
    {
      std::string t;
      if (is >> t) start_moves = std::stoi(t);
      if (is >> t) end_moves   = std::stoi(t);
      if (is >> t) step        = std::stoi(t);
      if (is >> t) count_each  = std::stoi(t);
      if (is >> t) output_file = t;
    }

    // 生成する手数リストを構築
    std::vector<int> move_counts;
    for (int m = start_moves; m <= end_moves; m += step) {
      if (m >= 7 && m % 2 == 1) move_counts.push_back(m);
    }

    if (move_counts.empty()) {
      sync_cout << "info string [tsume_batch_generate] エラー: 有効な手数がありません" << sync_endl;
      return;
    }

    sync_cout << "info string [tsume_batch_generate] 開始: "
              << start_moves << "〜" << end_moves << "手(step=" << step << ") × 各"
              << count_each << "問 → " << output_file << sync_endl;

    std::mt19937 rng(std::random_device{}());
    std::vector<komori::TsumeGeneratedProblem> all_found;

    g_tsume_gen_silent = true;

    for (int moves : move_counts) {
      sync_cout << "info string [tsume_batch_generate] ===== " << moves
                << "手詰め生成開始 (時間上限: "
                << komori::detail::ComputeTimeLimitMs(moves) << "ms/局面) =====" << sync_endl;

      const std::size_t before = all_found.size();
      GenerateProblemsForMoves(moves, count_each, rng, all_found, output_file);
      const int got = static_cast<int>(all_found.size() - before);

      sync_cout << "info string [tsume_batch_generate] " << moves << "手詰め: "
                << got << "/" << count_each << "問 完了" << sync_endl;
    }

    g_tsume_gen_silent = false;

    sync_cout << "info string [tsume_batch_generate] 合計 " << all_found.size()
              << " 問生成完了" << sync_endl;

    if (!all_found.empty() && output_file.empty()) {
      komori::SaveTsumeProblems(all_found, output_file);
    }
    return;
  }

  // 不明なサブコマンド
  if (!token.empty()) {
    sync_cout << "info string [user] 不明なサブコマンド: " << token << sync_endl;
    sync_cout << "info string [user] 使い方:" << sync_endl;
    sync_cout << "info string [user]   user tsume_answer [部分問題ms] [全体df-pn ms] [作意USI手順...]" << sync_endl;
    sync_cout << "info string [user]   user tsume_verify [上限手数] [上限ノード数] [既知USI手順...]" << sync_endl;
    sync_cout << "info string [user]   user tsume_save <上限手数> <上限ノード数> <JSONLファイル> [既知USI手順...]" << sync_endl;
    sync_cout << "info string [user]   user tsume_load <JSONLファイル> [上限手数] [上限ノード数]" << sync_endl;
    sync_cout << "info string [user]   user tsume_extend <件数> <上限手数> <上限ノード数> <JSONLファイル>" << sync_endl;
    sync_cout << "info string [user]   user tsume_solve" << sync_endl;
    sync_cout << "info string [user]   user tsume_generate <手数> [問題数] [ファイル]" << sync_endl;
    sync_cout << "info string [user]   user tsume_batch_generate <開始手数> <終了手数> <ステップ> [各問題数] [ファイル]" << sync_endl;
  }
}

// USIに追加オプションを設定したいときは、この関数を定義すること。
void USI::extra_option(USI::OptionsMap& o) {
  komori::EngineOption::Init(o);
}

void Search::init() {}

void Search::clear() {
  Threads.main()->wait_for_search_finished();
  Threads.clear();

  if (!g_path_key_init_flag) {
    g_path_key_init_flag = true;
    komori::PathKeyInit();
  }
  g_option.Reload(Options);

#if defined(USE_DEEP_DFPN)
  auto d = g_option.deep_dfpn_d_;
  auto e = g_option.deep_dfpn_e_;
  komori::DeepDfpnInit(d, e);
#endif

  Searcher().Init(g_option, Threads.size());
}

void MainThread::search() {
  const bool is_mate_search  = Search::Limits.mate != 0;
  const bool is_root_or_node = IsPosOrNode(rootPos);

  Searcher().NewSearch(rootPos, is_root_or_node);
  Threads.start_searching();
  Thread::search();
  Threads.stop = true;
  Threads.wait_for_search_finished();

  Move best_move = MOVE_NONE;
  if (g_search_result == komori::NodeState::kProven) {
    auto best_moves = Searcher().BestMoves();
    PrintResult(is_mate_search, LoseKind::kMate, komori::ToString(best_moves));
    if (!best_moves.empty()) best_move = best_moves[0];
  } else {
    if (g_search_result == komori::NodeState::kDisproven ||
        g_search_result == komori::NodeState::kRepetition) {
      PrintResult(is_mate_search, LoseKind::kNoMate);
    } else {
      PrintResult(is_mate_search, LoseKind::kTimeout);
    }
  }

  if (Search::Limits.mate == 0) {
    while (!Threads.stop && Search::Limits.infinite) Tools::sleep(1);
    sync_cout << "bestmove " << (best_move == MOVE_NONE ? "resign" : USI::move(best_move))
              << sync_endl;
    return;
  }
}

void Thread::search() {
  komori::InitializeThread(id(), Threads.size());
  const auto result = Searcher().Search(rootPos, IsPosOrNode(rootPos));
  if (id() == 0) g_search_result = result;
}

#endif  // USER_ENGINE
