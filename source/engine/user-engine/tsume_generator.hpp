/**
 * @file tsume_generator.hpp
 * @brief 詰将棋の生成・保存機能
 *
 * 詰将棋ルールに従った局面をランダムに生成し、SFEN 形式でファイルへ保存する。
 *
 * ルール準拠:
 *   - 攻め方は毎手王手のみ（df-pn ソルバーが OR ノードで王手のみ生成）
 *   - 打ち歩詰め禁止（合法手生成で除外済み）
 *   - 連続王手の千日手は攻め方の負け（repetition として検出済み）
 *   - 受け方は最善手で応じる（AND ノードで全回避手を評価）
 *   - USI 準拠の SFEN 形式で保存
 *
 * 将棋の駒数制約（全局面を通じた合計枚数）:
 *   歩(P): 18枚, 香(L): 4枚, 桂(N): 4枚, 銀(S): 4枚,
 *   金(G): 4枚, 角(B): 2枚, 飛(R): 2枚, 王(K): 2枚
 */
#ifndef KOMORI_TSUME_GENERATOR_HPP_
#define KOMORI_TSUME_GENERATOR_HPP_

#include <array>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "../../misc.h"
#include "../../position.h"
#include "../../usi.h"

namespace komori {

/// 生成した詰将棋問題
struct TsumeGeneratedProblem {
  std::string sfen;                 ///< USI 準拠 SFEN 文字列
  int mate_in;                      ///< 詰み手数
  std::vector<std::string> solution; ///< 解答手順（USI move 文字列）
  std::string record_json;          ///< 完全検証・評価を含む JSON record
};

namespace detail {

// ---------------------------------------------------------------------------
// 将棋全駒数制約（盤上＋持ち駒の合計、両先後合わせた全局面の最大値）
// ---------------------------------------------------------------------------
// 駒種インデックス: 0=P, 1=L, 2=N, 3=S, 4=G, 5=B, 6=R
inline constexpr std::array<char, 7> kPieceChars    = {'P', 'L', 'N', 'S', 'G', 'B', 'R'};
inline constexpr std::array<int, 7>  kPieceMaxTotal = {18, 4, 4, 4, 4, 2, 2};
//                                                      P   L  N  S  G  B  R

/**
 * @brief 目標手数に応じたノード数上限を返す
 * @param target_moves 目標詰み手数
 * @return 探索ノード数上限
 */
inline std::uint64_t ComputeNodesLimit(int target_moves) {
  if (target_moves <= 5)  return    500'000ULL;
  if (target_moves <= 9)  return 50'000'000ULL;  // 9手39枚制: 受け方予備駒多くEVノード増加（5倍再試行で250M）
  if (target_moves <= 11) return  3'000'000ULL;
  if (target_moves <= 21) return  4'000'000ULL;
  if (target_moves <= 31) return 12'000'000ULL;
  return 25'000'000ULL;
}

/**
 * @brief 目標手数に応じた探索時間上限を返す（ミリ秒）
 * @param target_moves 目標詰み手数
 * @return 1局面あたりの探索時間上限[ms]
 */
inline std::uint64_t ComputeTimeLimitMs(int target_moves) {
  if (target_moves <= 5)  return    500ULL;
  if (target_moves <= 9)  return  4'000ULL;  // 9手: EV に余裕を持たせる
  if (target_moves <= 11) return  1'500ULL;
  if (target_moves <= 21) return  4'000ULL;
  if (target_moves <= 31) return 12'000ULL;
  return 25'000ULL;
}

/**
 * @brief 目標手数に応じた攻め方の最大駒数を返す（盤上＋持ち駒の合計）
 * @param target_moves 目標詰み手数
 * @return 攻め方の駒数上限
 */
inline int ComputeMaxPieces(int target_moves) {
  if (target_moves <= 3)  return 2;
  if (target_moves <= 7)  return 3;
  if (target_moves <= 9)  return 5;   // 9手: 4-5駒で構造的制約を確保（少なすぎると逆算失敗）
  if (target_moves <= 15) return 4;
  if (target_moves <= 25) return 5;
  return 6;
}

/**
 * @brief 盤上配置が合法かを確認する（行き所なし駒チェック）
 * @param piece_char 駒文字（'P','L','N' など大文字=先手=攻め方）
 * @param sfen_rank  SFEN 段インデックス（0=1段目=先手の成り位置）
 * @return true: 配置可能、false: 非合法（行き所なし）
 *
 * 先手（攻め方）の駒が動けなくなる配置を避ける:
 *   歩(P)・香(L): 1段目（SFEN rank 0）は非合法
 *   桂(N):        1〜2段目（SFEN rank 0〜1）は非合法
 */
inline bool IsValidBoardPlacement(char piece_char, int sfen_rank) {
  if (piece_char == 'P' || piece_char == 'L') {
    return sfen_rank != 0;
  }
  if (piece_char == 'N') {
    return sfen_rank >= 2;
  }
  return true;
}

/**
 * @brief ランダムな詰将棋候補の SFEN 文字列を生成する
 * @param rng          乱数生成器
 * @param target_moves 目標詰み手数（駒数の調整に使用）
 * @return 候補局面の SFEN 文字列
 *
 * 生成ルール:
 *   - 受け方の玉（k）を盤上のランダムな位置に配置（位置バイアスなし）
 *   - 攻め方は将棋全駒種（歩・香・桂・銀・金・角・飛）を使用
 *   - 各駒種の合計枚数は将棋の駒数制限を超えない
 *   - 行き所なし駒（歩・香の1段目、桂の1〜2段目）は盤上に配置しない
 *   - 二歩（同一筋の歩が2枚以上）を避ける
 *   - 残り全駒（攻め方が使用しない分）を受け方持ち駒に回す（全40枚を盤上・持ち駒で完全使用）
 *   - 攻め方の玉は配置しない（詰将棋の標準形）
 */
inline std::string GenerateCandidateSfen(std::mt19937& rng, int target_moves = 3) {
  // 9×9 盤面（'.' = 空、大文字 = 先手=攻め方、小文字 = 後手=受け方）
  std::array<std::array<char, 9>, 9> board{};
  for (auto& rank : board) rank.fill('.');

  // 各筋の先手歩の有無（二歩チェック用）
  std::array<bool, 9> pawn_in_file{};
  pawn_in_file.fill(false);

  // 受け方の玉をランダムに配置（位置バイアスなし）
  std::uniform_int_distribution<int> rank_dist(0, 8);
  std::uniform_int_distribution<int> file_dist(0, 8);

  // 9手詰めは逃げ道が必要なため、玉を上段（1-4段目）・任意筋に配置する。
  // 3手詰め以下は端玉が有効なため左右端列にもバイアス。
  const int king_rank = [&]() {
    if (target_moves >= 9) {
      // 上4段（rank 0-3）に60%のバイアス（逃げ道を確保）
      std::bernoulli_distribution top_bias(0.60);
      return top_bias(rng) ? static_cast<int>(rng() % 4) : rank_dist(rng);
    }
    std::bernoulli_distribution edge_bias(0.75);
    return edge_bias(rng) ? static_cast<int>(rng() % 3) : rank_dist(rng);
  }();
  const int king_file = [&]() {
    if (target_moves >= 9) {
      return file_dist(rng);  // 9手以上: 筋バイアスなし（逃げ道を横方向に確保）
    }
    std::bernoulli_distribution edge_bias(0.75);
    return edge_bias(rng) ? static_cast<int>(rng() % 3) : file_dist(rng);
  }();
  board[king_rank][king_file] = 'k';

  // 目標手数に応じた攻め方の駒数
  const int max_pieces = ComputeMaxPieces(target_moves);
  const int min_pieces = (target_moves >= 9) ? 2 : 1;  // 9手以上は最低2駒
  std::uniform_int_distribution<int> num_pieces_dist(min_pieces, max_pieces);
  const int num_pieces = num_pieces_dist(rng);

  std::uniform_int_distribution<int> piece_type_dist(0, static_cast<int>(kPieceChars.size()) - 1);
  std::uniform_int_distribution<int> hand_or_board_dist(0, 1);

  // 駒種ごとの使用枚数を追跡（将棋の駒数制限）
  std::array<int, 7> used_count{};
  used_count.fill(0);
  std::string hand_str;

  // 大駒（飛・角）は1枚まで: 複数あると非限定詰みになりやすい
  int major_pieces_used = 0;
  static constexpr int kMaxMajorPieces = 1;  // B=index5, R=index6
  // 金は2枚まで: 3枚以上は変同の主原因（2枚は離れた役割の場合に有効）
  int gold_used = 0;
  static constexpr int kMaxGold = 2;  // G=index4

  for (int i = 0; i < num_pieces; ++i) {
    // 使用可能枚数が残っている駒種を選択
    int piece_idx = -1;
    for (int attempt = 0; attempt < 30; ++attempt) {
      int idx = piece_type_dist(rng);
      if (used_count[idx] < kPieceMaxTotal[idx]) {
        // 大駒制限
        const bool is_major = (idx == 5 || idx == 6);  // B=index5, R=index6
        if (is_major && major_pieces_used >= kMaxMajorPieces) continue;
        // 金制限
        if (idx == 4 && gold_used >= kMaxGold) continue;
        piece_idx = idx;
        break;
      }
    }
    if (piece_idx < 0) continue;

    char piece = kPieceChars[piece_idx];

    // 盤上 or 持ち駒のどちらに置くか決定
    // 歩は盤上に置くと二歩になりやすいので持ち駒優先にする
    // 攻め方持ち駒が解答で使われないと「駒余り」で不合格になるため、盤上を強く優先する
    const bool prefer_hand = (piece == 'P');
    std::bernoulli_distribution board_bias(0.85);
    bool place_on_board = (!prefer_hand) && board_bias(rng);

    if (place_on_board) {
      // 盤上に配置できるマスを探す
      // 王の近傍に配置しやすくする（詰将棋では攻め方が王に迫る構造が多い）
      std::bernoulli_distribution near_king_bias(0.65);
      const bool use_near_king = near_king_bias(rng);
      bool placed = false;
      for (int attempt = 0; attempt < 50 && !placed; ++attempt) {
        int r, f;
        if (use_near_king) {
          // 王から最大4マス以内にバイアス
          const int spread = (target_moves >= 9) ? 3 : 4;
          r = std::max(0, std::min(8, king_rank + static_cast<int>(rng() % (2*spread+1)) - spread));
          f = std::max(0, std::min(8, king_file + static_cast<int>(rng() % (2*spread+1)) - spread));
        } else {
          r = rank_dist(rng);
          f = file_dist(rng);
        }

        if (board[r][f] != '.') continue;
        if (!IsValidBoardPlacement(piece, r)) continue;
        // 二歩チェック
        if (piece == 'P' && pawn_in_file[f]) continue;

        board[r][f] = piece;
        if (piece == 'P') pawn_in_file[f] = true;
        placed = true;
      }
      if (!placed) {
        // 盤上に置けない場合は持ち駒へ
        place_on_board = false;
      }
    }

    if (!place_on_board) {
      hand_str += piece;
    }

    ++used_count[piece_idx];
    if (piece_idx == 5 || piece_idx == 6) ++major_pieces_used;
    if (piece_idx == 4) ++gold_used;
  }

  // Add a small number of generated defender pieces around the king.  Purely
  // sparse attacker-only positions leave almost every escape and interposition
  // available and very rarely survive uniqueness/full-width verification.
  // These pieces are part of the same random construction (not a library seed)
  // and are charged against the physical 39-piece set below.
  const int max_defenders = target_moves >= 7 ? 3 : 1;
  const int min_defenders = 0;
  std::uniform_int_distribution<int> defender_count_dist(min_defenders, max_defenders);
  std::uniform_int_distribution<int> defender_type_dist(0, 4);  // P,L,N,S,G
  const int defender_count = defender_count_dist(rng);
  for (int i = 0; i < defender_count; ++i) {
    int piece_idx = -1;
    for (int attempt = 0; attempt < 20; ++attempt) {
      const int idx = defender_type_dist(rng);
      if (used_count[idx] < kPieceMaxTotal[idx]) { piece_idx = idx; break; }
    }
    if (piece_idx < 0) continue;

    const char piece = static_cast<char>(std::tolower(
        static_cast<unsigned char>(kPieceChars[piece_idx])));
    bool placed = false;
    for (int attempt = 0; attempt < 40 && !placed; ++attempt) {
      const int r = std::max(0, std::min(8, king_rank + static_cast<int>(rng() % 7) - 3));
      const int f = std::max(0, std::min(8, king_file + static_cast<int>(rng() % 7) - 3));
      if (board[r][f] != '.') continue;
      // White pawns/lances cannot remain on rank 9; white knights cannot
      // remain on ranks 8 or 9.
      if ((piece == 'p' || piece == 'l') && r == 8) continue;
      if (piece == 'n' && r >= 7) continue;
      // 二歩チェック: 後手歩は同筋にすでに歩があれば配置不可
      if (piece == 'p') {
        bool nifu = false;
        for (int rr = 0; rr < 9; ++rr) if (board[rr][f] == 'p') { nifu = true; break; }
        if (nifu) continue;
      }
      board[r][f] = piece;
      placed = true;
    }
    if (placed) ++used_count[piece_idx];
  }

  // SFEN 文字列を構築（USI 準拠フォーマット）
  // フォーマット: <盤面9段/区切り> <手番 b> <持ち駒(先手大文字+後手小文字)> <手数>
  std::string sfen;
  sfen.reserve(100);
  for (int rank = 0; rank < 9; ++rank) {
    if (rank > 0) sfen += '/';
    int empty_count = 0;
    for (int file = 0; file < 9; ++file) {
      const char c = board[rank][file];
      if (c == '.') {
        ++empty_count;
      } else {
        if (empty_count > 0) {
          sfen += std::to_string(empty_count);
          empty_count = 0;
        }
        sfen += c;
      }
    }
    if (empty_count > 0) sfen += std::to_string(empty_count);
  }
  sfen += " b ";
  sfen += hand_str.empty() ? "-" : hand_str;
  sfen += " 1";
  return sfen;
}

/**
 * @brief 9手詰め特化の構造化シード生成（N+S+B_hand パターン）
 *
 * 既知の有効パターン: 桂(N)を王の直下(kr+1, kf)、銀(S)を(kr+2, kf±1)、
 * 角(B)+金(G)を持ち駒。この配置が9手詰め小手合い局面を生みやすい。
 */
inline std::string GenerateStructuredSeed9ply(std::mt19937& rng) {
  std::array<std::array<char, 9>, 9> board{};
  for (auto& rank : board) rank.fill('.');
  std::uniform_int_distribution<int> file_dist(0, 8);

  // 王を2-3段目（kr=1,2）に配置：kr=0 では N が行き所なし（桂は2段跳び必須）
  const int kr = 1 + static_cast<int>(rng() % 2);  // rank 1 or 2
  // 王はファイル2-6（0-indexed）に配置：端（0,1,7,8）では逃げ場が少なく手数が短くなりすぎる
  const int kf = 2 + static_cast<int>(rng() % 5);  // file 2-6 (columns 7-3 in USI notation)
  board[kr][kf] = 'k';

  std::string hand_str;

  // パターン選択（4種）
  const int pattern = static_cast<int>(rng() % 4);

  if (pattern == 0 || pattern == 1) {
    // Pattern A: 桂(N)を王の直下 + 銀(S)を2段下・横 + 角(B)+金(G)持ち駒（既知最良構造）
    // N は王の真下（kr+1, kf）: 桂は2段跳びで kr-1 段目を攻撃 → 王の逃げ先を封じる
    if (kr + 1 <= 8 && board[kr + 1][kf] == '.') {
      board[kr + 1][kf] = 'N';
    }
    // S は王から2段下・左右どちらか
    const int sf_delta = (rng() % 2 == 0) ? 1 : -1;
    const int sf = kf + sf_delta;
    if (kr + 2 <= 8 && sf >= 0 && sf < 9 && board[kr + 2][sf] == '.') {
      board[kr + 2][sf] = 'S';
    } else if (kr + 2 <= 8 && kf >= 0 && board[kr + 2][kf] == '.') {
      board[kr + 2][kf] = 'S';
    }
    // B を持ち駒に（必須）
    hand_str += 'B';
    // G を持ち駒に（pattern 0 では必須、pattern 1 では50%確率）
    if (pattern == 0 || rng() % 2 == 0) { hand_str += 'G'; }
  } else if (pattern == 2) {
    // Pattern B: 桂(N)を王の直下 + 金(G)を王の斜め下 + 角(B)持ち駒
    if (kr + 1 <= 8 && board[kr + 1][kf] == '.') {
      board[kr + 1][kf] = 'N';
    }
    const int gf_delta = (rng() % 2 == 0) ? 1 : -1;
    const int gf = kf + gf_delta;
    if (kr + 1 <= 8 && gf >= 0 && gf < 9 && board[kr + 1][gf] == '.') {
      board[kr + 1][gf] = 'G';
    } else if (kr + 2 <= 8 && board[kr + 2][kf] == '.') {
      board[kr + 2][kf] = 'G';
    }
    hand_str += 'B';
    if (rng() % 2 == 0) { hand_str += 'G'; }
  } else {
    // Pattern C: 2桂(N)で王の両脇 + 銀(S)を下 + 角(B)+金(G)持ち駒
    const int nf1 = kf - 1, nf2 = kf + 1;
    if (kr + 2 <= 8 && nf1 >= 0 && board[kr + 2][nf1] == '.') {
      board[kr + 2][nf1] = 'N';
    }
    if (kr + 2 <= 8 && nf2 < 9 && board[kr + 2][nf2] == '.') {
      board[kr + 2][nf2] = 'N';
    }
    // 銀を追加（任意位置）
    const int sf_delta = (rng() % 2 == 0) ? 1 : -1;
    const int sf = kf + sf_delta;
    if (kr + 1 <= 8 && sf >= 0 && sf < 9 && board[kr + 1][sf] == '.') {
      board[kr + 1][sf] = 'S';
    }
    hand_str += 'B';
    hand_str += 'G';
  }

  // SFEN構築
  std::string sfen;
  sfen.reserve(80);
  for (int rank = 0; rank < 9; ++rank) {
    if (rank > 0) sfen += '/';
    int empty_count = 0;
    for (int file = 0; file < 9; ++file) {
      const char c = board[rank][file];
      if (c == '.') { ++empty_count; }
      else { if (empty_count > 0) { sfen += std::to_string(empty_count); empty_count = 0; } sfen += c; }
    }
    if (empty_count > 0) sfen += std::to_string(empty_count);
  }
  sfen += " b ";
  sfen += hand_str.empty() ? "-" : hand_str;
  sfen += " 1";
  return sfen;
}

// ---------------------------------------------------------------------------
// SFEN board utilities for canonicalization (正規形), 邪魔駒確認, 手数伸ばし
// ---------------------------------------------------------------------------

// board[rank][file]: rank 0=1段目(top), file 0=9筋(right). Cell: "" empty, "S", "+S", etc.
using SfenBoard = std::array<std::array<std::string, 9>, 9>;

inline void ParseSfenBoard(const std::string& s, SfenBoard& b) {
  for (auto& row : b) row.fill("");
  int r = 0, f = 0;
  bool promo = false;
  for (char c : s) {
    if (c == '/')                                      { ++r; f = 0; promo = false; }
    else if (c == '+')                                 { promo = true; }
    else if (std::isdigit(static_cast<unsigned char>(c))) { f += c - '0'; promo = false; }
    else { b[r][f++] = (promo ? "+" : "") + std::string(1, c); promo = false; }
  }
}

inline std::string BuildSfenBoard(const SfenBoard& b) {
  std::string s;
  for (int r = 0; r < 9; ++r) {
    if (r > 0) s += '/';
    int e = 0;
    for (int f = 0; f < 9; ++f) {
      if (b[r][f].empty()) { ++e; }
      else { if (e) { s += std::to_string(e); e = 0; } s += b[r][f]; }
    }
    if (e) s += std::to_string(e);
  }
  return s;
}

/** Restore the defender's implicit reserve from the orthodox piece set.
 * Standard SFEN itself has no notion of an implicit hand, therefore the
 * returned value is another, fully explicit SFEN string.
 */
inline std::string CompleteDefenderReserve(const std::string& sfen, bool double_king) {
  std::istringstream in(sfen);
  std::string board_text, turn, hands, move_no;
  if (!(in >> board_text >> turn >> hands >> move_no)) return sfen;
  SfenBoard board; ParseSfenBoard(board_text, board);
  std::array<int, 7> used{};
  auto index_of = [](char c) {
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(c)))) {
      case 'P': return 0; case 'L': return 1; case 'N': return 2; case 'S': return 3;
      case 'G': return 4; case 'B': return 5; case 'R': return 6; default: return -1;
    }
  };
  int kings = 0;
  for (const auto& row : board) for (const auto& cell : row) if (!cell.empty()) {
    const char c = cell.back();
    if (std::toupper(static_cast<unsigned char>(c)) == 'K') ++kings;
    else if (int i = index_of(c); i >= 0) ++used[i];
  }
  // 攻め方の持ち駒（大文字）を保持し、受け方持ち駒は再計算して正規化する
  std::array<int, 7> attacker_hand_count{};
  if (hands != "-") {
    int count = 0;
    for (char c : hands) {
      if (std::isdigit(static_cast<unsigned char>(c))) { count = count * 10 + c - '0'; continue; }
      const int n = count ? count : 1; count = 0;
      // 受け方の持ち駒は下で盤上・攻め方持ち駒の残りとして作り直すので数えない
      if (int i = index_of(c); i >= 0 && std::isupper(static_cast<unsigned char>(c))) {
        used[i] += n;
        attacker_hand_count[i] += n;
      }
    }
  }
  // A single-king composition uses 39 pieces; double-king uses all 40.
  const int expected_kings = double_king ? 2 : 1;
  if (kings != expected_kings) return sfen;
  // 攻め方持ち駒（大文字）を先に出力、次に残り全駒を受け方持ち駒として出力
  std::string new_hands;
  for (int i = 0; i < 7; ++i) {
    if (attacker_hand_count[i] > 1) new_hands += std::to_string(attacker_hand_count[i]);
    if (attacker_hand_count[i] > 0) new_hands += kPieceChars[i];
  }
  for (int i = 0; i < 7; ++i) {
    const int remaining = kPieceMaxTotal[i] - used[i];
    if (remaining < 0) return sfen;  // invalid material
    if (remaining > 1) new_hands += std::to_string(remaining);
    if (remaining > 0) new_hands += static_cast<char>(std::tolower(static_cast<unsigned char>(kPieceChars[i])));
  }
  return board_text + " " + turn + " " + (new_hands.empty() ? "-" : new_hands) + " " + move_no;
}

/// surplus な攻め方手持ち駒（解で一度も使われなかった駒種）を受け方持ち駒へ移す
/// 返り値: 変更後 SFEN。変更なし or 不正な場合は空文字列
inline std::string StripSurplusAttackerHand(
    const std::string& sfen,
    Position& pos,
    const std::vector<Move>& solution) {
  const Color attacker = pos.side_to_move();
  const Hand initial_hand = pos.hand_of(attacker);
  if (initial_hand == 0) return {};
  std::vector<StateInfo> states(solution.size());
  for (std::size_t i = 0; i < solution.size(); ++i) pos.do_move(solution[i], states[i]);
  const Hand final_hand = pos.hand_of(attacker);
  for (auto it = solution.rbegin(); it != solution.rend(); ++it) pos.undo_move(*it);

  // surplus な駒種: final >= initial なら一度も使われていない
  static constexpr PieceType kPT[] = {PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK};
  bool any_surplus = false;
  for (auto pt : kPT) if (hand_count(final_hand, pt) >= hand_count(initial_hand, pt)) { any_surplus = true; break; }
  if (!any_surplus) return {};

  // SFEN を解析して手持ち文字列を再構築
  std::istringstream ss(sfen);
  std::string bd, tr, hs, mn;
  if (!(ss >> bd >> tr >> hs >> mn)) return {};
  std::map<char, int> ahand, dhand;
  int cnt = 0;
  for (char c : hs) {
    if (c == '-') continue;
    if (std::isdigit((unsigned char)c)) { cnt = cnt * 10 + (c - '0'); continue; }
    int n = cnt ? cnt : 1; cnt = 0;
    if (std::isupper((unsigned char)c)) ahand[c] += n;
    else dhand[(char)std::toupper((unsigned char)c)] += n;
  }
  // surplus な駒を受け方手持ちへ移す
  auto piece_char_upper = [](PieceType pt) -> char {
    switch (pt) {
      case PAWN:   return 'P'; case LANCE:  return 'L'; case KNIGHT: return 'N';
      case SILVER: return 'S'; case GOLD:   return 'G'; case BISHOP: return 'B';
      case ROOK:   return 'R'; default: return 0;
    }
  };
  bool moved = false;
  for (auto pt : kPT) {
    if (hand_count(final_hand, pt) >= hand_count(initial_hand, pt)) {
      char c = piece_char_upper(pt);
      int n = ahand[c];
      if (n > 0) { dhand[c] += n; ahand[c] = 0; moved = true; }
    }
  }
  if (!moved) return {};
  std::string new_hs;
  for (char c : {'R','B','G','S','N','L','P'}) {
    if (ahand[c] > 0) { if (ahand[c] > 1) new_hs += std::to_string(ahand[c]); new_hs += c; }
  }
  for (char c : {'R','B','G','S','N','L','P'}) {
    char lc = (char)std::tolower((unsigned char)c);
    if (dhand[c] > 0) { if (dhand[c] > 1) new_hs += std::to_string(dhand[c]); new_hs += lc; }
  }
  return bd + " " + tr + " " + (new_hs.empty() ? "-" : new_hs) + " " + mn;
}

inline SfenBoard TransformBoard(const SfenBoard& b, bool flip, int sf, int sr) {
  SfenBoard out; for (auto& row : out) row.fill("");
  for (int r = 0; r < 9; ++r)
    for (int f = 0; f < 9; ++f) {
      if (b[r][f].empty()) continue;
      int nf = (flip ? 8 - f : f) - sf, nr = r - sr;
      if (nf >= 0 && nf <= 8 && nr >= 0 && nr <= 8) out[nr][nf] = b[r][f];
    }
  return out;
}

// USI square conventions: file digit '1'-'9' (1=1筋, 9=9筋), rank letter 'a'-'i' (a=1段)
// array_file = 9 - usi_file_digit,  array_rank = usi_rank_char - 'a'
inline std::string TransformUsiMove(const std::string& mv, bool flip, int sf, int sr) {
  auto af = [](char c) { return 9 - (c - '0'); };
  auto ar = [](char c) { return c - 'a'; };
  auto uf = [](int f)  { return static_cast<char>('0' + (9 - f)); };
  auto ur = [](int r)  { return static_cast<char>('a' + r); };
  auto xf = [&](int f) { return (flip ? 8 - f : f) - sf; };
  auto xr = [&](int r) { return r - sr; };

  if (mv.size() >= 4 && mv[1] == '*')  // drop: "R*5e"
    return std::string(1, mv[0]) + "*" + uf(xf(af(mv[2]))) + ur(xr(ar(mv[3])));
  // normal: "7g7f" or "7g7f+"
  std::string res;
  res += uf(xf(af(mv[0]))); res += ur(xr(ar(mv[1])));
  res += uf(xf(af(mv[2]))); res += ur(xr(ar(mv[3])));
  if (mv.size() > 4) res += mv[4];
  return res;
}

struct CanonicalResult {
  std::string sfen;
  std::vector<std::string> solution;
};

/// 正規形変換: 左右反転 × シフトの組み合わせで受け方玉が最も右上になる版を選ぶ
inline CanonicalResult CanonicalizePosition(const std::string& sfen,
                                             const std::vector<std::string>& sol) {
  std::istringstream ss(sfen);
  std::string board_str, turn, hand_str, move_num;
  ss >> board_str >> turn >> hand_str >> move_num;

  SfenBoard board; ParseSfenBoard(board_str, board);

  int best_kf = 8, best_kr = 8;
  bool best_flip = false; int best_sf = 0, best_sr = 0;

  for (bool flip : {false, true}) {
    int min_f = 8, min_r = 8;
    for (int r = 0; r < 9; ++r)
      for (int f = 0; f < 9; ++f)
        if (!board[r][f].empty()) {
          min_f = std::min(min_f, flip ? 8 - f : f);
          min_r = std::min(min_r, r);
        }
    auto tb = TransformBoard(board, flip, min_f, min_r);
    for (int r = 0; r < 9; ++r)
      for (int f = 0; f < 9; ++f)
        if (tb[r][f] == "k") {
          if (f + r < best_kf + best_kr || (f + r == best_kf + best_kr && f < best_kf)) {
            best_kf = f; best_kr = r;
            best_flip = flip; best_sf = min_f; best_sr = min_r;
          }
        }
  }

  auto tb = TransformBoard(board, best_flip, best_sf, best_sr);
  CanonicalResult result;
  result.sfen = BuildSfenBoard(tb) + " " + turn + " " + hand_str + " " + move_num;
  for (const auto& mv : sol)
    result.solution.push_back(TransformUsiMove(mv, best_flip, best_sf, best_sr));
  return result;
}

}  // namespace detail

/**
 * @brief 生成した詰将棋問題を USI 準拠 SFEN 形式でファイルに追記保存する
 * @param problems 保存する問題リスト
 * @param filename 出力ファイル名
 *
 * 出力フォーマット（1行1問題）:
 *   sfen <SFEN> ; moves <手順> ; mate_in <手数>
 */
inline std::vector<TsumeGeneratedProblem> LoadTsumeProblems(const std::string& filename) {
  std::vector<TsumeGeneratedProblem> result;
  std::ifstream ifs(filename);
  if (!ifs) return result;
  std::string line;
  while (std::getline(ifs, line)) {
    if (line.empty() || line.rfind("sfen ", 0) != 0) continue;
    TsumeGeneratedProblem p;
    std::istringstream ss(line.substr(5));
    std::string tok;
    // SFENは4トークン: board turn hand moveno
    std::string board, turn, hand, moveno;
    ss >> board >> turn >> hand >> moveno;
    p.sfen = board + " " + turn + " " + hand + " " + moveno;
    while (ss >> tok) {
      if (tok == ";") continue;
      if (tok == "moves") { while (ss >> tok) { if (tok == ";") break; p.solution.push_back(tok); } }
      else if (tok == "mate_in") { ss >> p.mate_in; }
    }
    if (!p.sfen.empty()) result.push_back(std::move(p));
  }
  return result;
}

inline void SaveTsumeProblems(const std::vector<TsumeGeneratedProblem>& problems,
                               const std::string& filename) {
  std::ofstream ofs(filename, std::ios::app);
  if (!ofs) {
    sync_cout << "info string エラー: ファイルを開けません: " << filename << sync_endl;
    return;
  }
  for (const auto& p : problems) {
    ofs << "sfen " << p.sfen << " ; moves";
    for (const auto& m : p.solution) ofs << ' ' << m;
    ofs << " ; mate_in " << p.mate_in << '\n';
  }
  std::ofstream json(filename + ".jsonl", std::ios::app | std::ios::binary);
  if (json) for (const auto& p : problems) if (!p.record_json.empty()) json << p.record_json << '\n';
  sync_cout << "info string " << problems.size() << " 問題を保存: " << filename << sync_endl;
}

}  // namespace komori

#endif  // KOMORI_TSUME_GENERATOR_HPP_
