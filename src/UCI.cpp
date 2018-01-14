/*
 Stockfish, a UCI chess playing engine derived from Glaurung 2.1
 Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
 Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
 Copyright (C) 2015-2017 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad
 
 Stockfish is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 Stockfish is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

#include "Movegen.h"
#include "Position.h"
#include "Training.h"
#include "TTable.h"
#include "UCI.h"
#include "UCTSearch.h"

using namespace std;

// FEN string of the initial position, normal chess
const char* StartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

enum SyncCout { IO_LOCK, IO_UNLOCK };
std::ostream& operator<<(std::ostream&, SyncCout);

#define sync_cout std::cout << IO_LOCK
#define sync_endl std::endl << IO_UNLOCK

/// Used to serialize access to std::cout to avoid multiple threads writing at
/// the same time.

std::ostream& operator<<(std::ostream& os, SyncCout sc) {
  static mutex m;

  if (sc == IO_LOCK)
      m.lock();

  if (sc == IO_UNLOCK)
      m.unlock();

  return os;
}

namespace {

  // position() is called when engine receives the "position" UCI command.
  // The function sets up the position described in the given FEN string ("fen")
  // or the starting position ("startpos") and then makes the moves given in the
  // following move list ("moves").

  void position(BoardHistory& bh, istringstream& is) {

    Move m;
    string token, fen;

    is >> token;

    if (token == "startpos")
    {
        fen = StartFEN;
        is >> token; // Consume "moves" token if any
    }
    else if (token == "fen")
        while (is >> token && token != "moves")
            fen += token + " ";
    else
        return;

    bh.set(fen);

    // Parse move list (if any)
    while (is >> token && (m = UCI::to_move(bh.cur(), token)) != MOVE_NONE)
        bh.do_move(m);
  }


  // setoption() is called when engine receives the "setoption" UCI command. The
  // function updates the UCI option ("name") to the given value ("value").

  void setoption(istringstream& is) {

    string token, name, value;

    is >> token; // Consume "name" token

    // Read option name (can contain spaces)
    while (is >> token && token != "value")
        name += string(" ", name.empty() ? 0 : 1) + token;

    // Read option value (can contain spaces)
    while (is >> token)
        value += string(" ", value.empty() ? 0 : 1) + token;

    sync_cout << "No such option: " << name << sync_endl;
  }


  // go() is called when engine receives the "go" UCI command. The function sets
  // the thinking time and other parameters from the input string, then starts
  // the search.

  void go(BoardHistory& bh, istringstream& is) {

    string token;
    /*
    bool ponderMode = false;

    limits.startTime = now(); // As early as possible!

    while (is >> token)
        if (token == "wtime")     is >> limits.time[WHITE];
        else if (token == "btime")     is >> limits.time[BLACK];
        else if (token == "winc")      is >> limits.inc[WHITE];
        else if (token == "binc")      is >> limits.inc[BLACK];
        else if (token == "movestogo") is >> limits.movestogo;
        else if (token == "depth")     is >> limits.depth;
        else if (token == "nodes")     is >> limits.nodes;
        else if (token == "movetime")  is >> limits.movetime;
        else if (token == "infinite")  limits.infinite = 1;
        else if (token == "ponder")    ponderMode = true;
    */

    // TODO(gary): This just does the search on the UI thread...
    auto search = std::make_unique<UCTSearch>(bh.shallow_clone());
    Move move = search->think();
    bh.do_move(move);
    printf("bestmove %s\n", UCI::move(move).c_str());
  }

// Return the score from the self-play game
int play_one_game(BoardHistory& bh) {
  for (int game_ply = 0; game_ply < 150; ++game_ply) {
    if (bh.cur().is_draw()) {
      return 0;
    }
    MoveList<LEGAL> moves(bh.cur());
    if (moves.size() == 0) {
      if (bh.cur().checkers()) {
        // Checkmate
        return bh.cur().side_to_move() == WHITE ? -1 : 1;
      } else {
        // Stalemate
        return 0;
      }
    }
    auto search = std::make_unique<UCTSearch>(bh.shallow_clone());
    Move move = search->think();

    bh.do_move(move);
  }

  // Game termination as draw
  return 0;
}

int play_one_game() {
  BoardHistory bh;
  bh.set(StartFEN);

  TTable::get()->clear();
  Training::clear_training();
  int game_score = play_one_game(bh);

  printf("%s\n", bh.pgn().c_str());
  printf("Score: %d\n", game_score);

  return game_score;
}

void generate_training_games(unsigned long long n = std::numeric_limits<unsigned long long>::max()) {
  auto chunker = OutputChunker{"data/training", true};
  for (unsigned long long i = 0; i < n; i++) {
    Training::dump_training(play_one_game(), chunker);
  }
}

} // namespace


/// UCI::loop() waits for a command from stdin, parses it and calls the appropriate
/// function. Also intercepts EOF from stdin to ensure gracefully exiting if the
/// GUI dies unexpectedly. When called with some command line arguments, e.g. to
/// run 'bench', once the command is executed the function returns immediately.
/// In addition to the UCI ones, also some additional debug commands are supported.

void UCI::loop(const std::string& start) {

  string token, cmd = start;

  BoardHistory bh;
  bh.set(StartFEN);

  do {
      if (start.empty() && !getline(cin, cmd)) // Block here waiting for input or EOF
          cmd = "quit";

      istringstream is(cmd);

      token.clear(); // Avoid a stale if getline() returns empty or blank line
      is >> skipws >> token;

      /*
      // The GUI sends 'ponderhit' to tell us the user has played the expected move.
      // So 'ponderhit' will be sent if we were told to ponder on the same move the
      // user has played. We should continue searching but switch from pondering to
      // normal search. In case Threads.stopOnPonderhit is set we are waiting for
      // 'ponderhit' to stop the search, for instance if max search depth is reached.
      if (    token == "quit"
          ||  token == "stop"
          || (token == "ponderhit" && Threads.stopOnPonderhit))
          Threads.stop = true;
      else if (token == "ponderhit")
          Threads.ponder = false; // Switch to normal search
      */

      if (token == "uci")
          sync_cout << "id name lczero\n"
                    << "uciok"  << sync_endl;

      else if (token == "setoption")  setoption(is);
      else if (token == "go")         go(bh, is);
      else if (token == "position")   position(bh, is);
      // else if (token == "ucinewgame") Search::clear();
      else if (token == "isready")    sync_cout << "readyok" << sync_endl;

      // Additional custom non-UCI commands, mainly for debugging
      else if (token == "train")   generate_training_games();
      //else if (token == "bench") bench(pos, is, states);
      //else if (token == "d")     sync_cout << pos << sync_endl;
      //else if (token == "eval")  sync_cout << Eval::trace(pos) << sync_endl;
      else
          sync_cout << "Unknown command: " << cmd << sync_endl;

  } while (token != "quit" && start.empty()); // Command line args are one-shot
}

/// UCI::square() converts a Square to a string in algebraic notation (g1, a7, etc.)

std::string UCI::square(Square s) {
    return std::string{ char('a' + file_of(s)), char('1' + rank_of(s)) };
}

/// UCI::move() converts a Move to a string in coordinate notation (g1f3, a7a8q).
/// The only special case is castling, where we print in the e1g1 notation in
/// normal chess mode, and in e1h1 notation in chess960 mode. Internally all
/// castling moves are always encoded as 'king captures rook'.

string UCI::move(Move m) {
    
    Square from = from_sq(m);
    Square to = to_sq(m);
    
    if (m == MOVE_NONE)
        return "(none)";
    
    if (m == MOVE_NULL)
        return "0000";
    
    if (type_of(m) == CASTLING)
        to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));
    
    string move = UCI::square(from) + UCI::square(to);
    
    if (type_of(m) == PROMOTION)
        move += " pnbrqk"[promotion_type(m)];
    
    return move;
}


/// UCI::to_move() converts a string representing a move in coordinate notation
/// (g1f3, a7a8q) to the corresponding legal Move, if any.

Move UCI::to_move(const Position& pos, string& str) { 
    for (const auto& m : MoveList<LEGAL>(pos))
        if (str == UCI::move(m))
            return m;
    
    return MOVE_NONE;
}

