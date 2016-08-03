#if (NT == PV)
#define PvNode 1
#define func(name) name##_PV
#else
#define PvNode 0
#define func(name) name##_NonPV
#endif

Value func(search)(Pos *pos, Stack *ss, Value alpha, Value beta,
                  Depth depth, int cutNode)
{
  int rootNode = PvNode && (ss-1)->ply == 0;

  assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
  assert(PvNode || (alpha == beta - 1));
  assert(DEPTH_ZERO < depth && depth < DEPTH_MAX);
  assert(!(PvNode && cutNode));

  Move pv[MAX_PLY+1], quietsSearched[64];
  State st;
  TTEntry *tte;
  Key posKey;
  Move ttMove, move, excludedMove, bestMove;
  Depth extension, newDepth, predictedDepth;
  Value bestValue, value, ttValue, eval, nullValue;
  int ttHit, inCheck, givesCheck, singularExtensionNode, improving;
  int captureOrPromotion, doFullDepthSearch, moveCountPruning;
  Piece moved_piece;
  int moveCount, quietCount;

  // Step 1. Initialize node
  Thread *thisThread = pos->thisThread;
  inCheck = pos_checkers();
  moveCount = quietCount =  ss->moveCount = 0;
  bestValue = -VALUE_INFINITE;
  ss->ply = (ss-1)->ply + 1;

  // Check for the available remaining time
  if (load_rlx(thisThread->resetCalls)) {
    store_rlx(thisThread->resetCalls, 0);
    thisThread->callsCnt = 0;
  }
  if (++thisThread->callsCnt > 4096) {
    for (size_t idx = 0; idx < Threads.num_threads; idx++)
      store_rlx(Threads.thread[idx]->resetCalls, 1);

    check_time();
  }

  // Used to send selDepth info to GUI
  if (PvNode && thisThread->maxPly < ss->ply)
    thisThread->maxPly = ss->ply;

  if (!rootNode) {
    // Step 2. Check for aborted search and immediate draw
    if (load_rlx(Signals.stop) || is_draw(pos) || ss->ply >= MAX_PLY)
      return ss->ply >= MAX_PLY && !inCheck ? evaluate(pos)
                                            : DrawValue[pos_stm()];

    // Step 3. Mate distance pruning. Even if we mate at the next move our
    // score would be at best mate_in(ss->ply+1), but if alpha is already
    // bigger because a shorter mate was found upward in the tree then
    // there is no need to search because we will never beat the current
    // alpha. Same logic but with reversed signs applies also in the
    // opposite condition of being mated instead of giving mate. In this
    // case return a fail-high score.
    alpha = max(mated_in(ss->ply), alpha);
    beta = min(mate_in(ss->ply+1), beta);
    if (alpha >= beta)
      return alpha;
  }

  assert(0 <= ss->ply && ss->ply < MAX_PLY);

  ss->currentMove = (ss+1)->excludedMove = bestMove = 0;
  ss->counterMoves = NULL;
  (ss+1)->skipEarlyPruning = 0;
  (ss+2)->killers[0] = (ss+2)->killers[1] = 0;

  // Step 4. Transposition table lookup. We don't want the score of a
  // partial search to overwrite a previous full search TT value, so we
  // use a different position key in case of an excluded move.
  excludedMove = ss->excludedMove;
  posKey = excludedMove ? pos_exclusion_key() : pos_key();
  tte = tt_probe(posKey, &ttHit);
  ttValue = ttHit ? value_from_tt(tte_value(tte), ss->ply) : VALUE_NONE;
  ttMove =  rootNode ? thisThread->rootMoves->move[thisThread->PVIdx].pv[0]
          : ttHit    ? tte_move(tte) : 0;

  // At non-PV nodes we check for an early TT cutoff
  if (  !PvNode
      && ttHit
      && tte_depth(tte) >= depth
      && ttValue != VALUE_NONE // Possible in case of TT access race
      && (ttValue >= beta ? (tte_bound(tte) & BOUND_LOWER)
                          : (tte_bound(tte) & BOUND_UPPER))) {
    ss->currentMove = ttMove; // Can be MOVE_NONE

    // If ttMove is quiet, update killers, history, counter move on TT hit
    if (ttValue >= beta && ttMove && !is_capture_or_promotion(pos, ttMove))
      update_stats(pos, ss, ttMove, depth, NULL, 0);

    return ttValue;
  }

  // Step 4a. Tablebase probe
  if (!rootNode && TB_Cardinality) {
    int piecesCnt = popcount(pieces());

    if (    piecesCnt <= TB_Cardinality
        && (piecesCnt <  TB_Cardinality || depth >= TB_ProbeDepth)
        &&  pos_rule50_count() == 0
        && !can_castle_cr(ANY_CASTLING)) {

      int found, v = TB_probe_wdl(pos, &found);

      if (found) {
        TB_Hits++;

        int drawScore = TB_UseRule50 ? 1 : 0;

        value =  v < -drawScore ? -VALUE_MATE + MAX_PLY + ss->ply
               : v >  drawScore ?  VALUE_MATE - MAX_PLY - ss->ply
                                :  VALUE_DRAW + 2 * v * drawScore;

        tte_save(tte, posKey, value_to_tt(value, ss->ply), BOUND_EXACT,
                 min(DEPTH_MAX - ONE_PLY, depth + 6 * ONE_PLY),
                 0, VALUE_NONE, tt_generation());

        return value;
      }
    }
  }

  // Step 5. Evaluate the position statically
  if (inCheck) {
    ss->staticEval = eval = VALUE_NONE;
    goto moves_loop;
  } else if (ttHit) {
    // Never assume anything on values stored in TT
    if ((ss->staticEval = eval = tte_eval(tte)) == VALUE_NONE)
      eval = ss->staticEval = evaluate(pos);

    // Can ttValue be used as a better position evaluation?
    if (ttValue != VALUE_NONE)
      if (tte_bound(tte) & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER))
        eval = ttValue;
  } else {
    eval = ss->staticEval =
    (ss-1)->currentMove != MOVE_NULL ? evaluate(pos)
                                     : -(ss-1)->staticEval + 2 * Tempo;

    tte_save(tte, posKey, VALUE_NONE, BOUND_NONE, DEPTH_NONE, MOVE_NONE,
             ss->staticEval, tt_generation());
  }

  if (ss->skipEarlyPruning)
    goto moves_loop;

  // Step 6. Razoring (skipped when in check)
  if (   !PvNode
      &&  depth < 4 * ONE_PLY
      &&  eval + razor_margin[depth] <= alpha
      &&  ttMove == MOVE_NONE) {

    if (   depth <= ONE_PLY
        && eval + razor_margin[3 * ONE_PLY] <= alpha)
      return qsearch_NonPV_false(pos, ss, alpha, beta, DEPTH_ZERO);

    Value ralpha = alpha - razor_margin[depth];
    Value v = qsearch_NonPV_false(pos, ss, ralpha, ralpha+1, DEPTH_ZERO);
    if (v <= ralpha)
      return v;
  }

  // Step 7. Futility pruning: child node (skipped when in check)
  if (   !rootNode
      &&  depth < 7 * ONE_PLY
      &&  eval - futility_margin(depth) >= beta
      &&  eval < VALUE_KNOWN_WIN  // Do not return unproven wins
      &&  pos_non_pawn_material(pos_stm()))
    return eval - futility_margin(depth);

  // Step 8. Null move search with verification search (is omitted in PV nodes)
  if (   !PvNode
      &&  depth >= 2 * ONE_PLY
      &&  eval >= beta
      && (ss->staticEval >= beta || depth >= 12 * ONE_PLY)
      &&  pos_non_pawn_material(pos_stm())) {

    ss->currentMove = MOVE_NULL;
    ss->counterMoves = NULL;

    assert(eval - beta >= 0);

    // Null move dynamic reduction based on depth and value
    Depth R = ((823 + 67 * depth) / 256 + min((eval - beta) / PawnValueMg, 3)) * ONE_PLY;

    do_null_move(pos, &st);
    (ss+1)->skipEarlyPruning = 1;
    nullValue = depth-R < ONE_PLY ? -qsearch_NonPV_false(pos, ss+1, -beta, -beta+1, DEPTH_ZERO)
                                  : - search_NonPV(pos, ss+1, -beta, -beta+1, depth-R, !cutNode);
    (ss+1)->skipEarlyPruning = 0;
    undo_null_move(pos);

    if (nullValue >= beta) {
      // Do not return unproven mate scores
      if (nullValue >= VALUE_MATE_IN_MAX_PLY)
         nullValue = beta;

      if (depth < 12 * ONE_PLY && abs(beta) < VALUE_KNOWN_WIN)
         return nullValue;

      // Do verification search at high depths
      ss->skipEarlyPruning = 1;
      Value v = depth-R < ONE_PLY ? qsearch_NonPV_false(pos, ss, beta-1, beta, DEPTH_ZERO)
                                  :  search_NonPV(pos, ss, beta-1, beta, depth-R, 0);
      ss->skipEarlyPruning = 0;

      if (v >= beta)
        return nullValue;
    }
  }

  // Step 9. ProbCut (skipped when in check)
  // If we have a very good capture (i.e. SEE > seeValues[captured_piece_type])
  // and a reduced search returns a value much above beta, we can (almost)
  // safely prune the previous move.
  if (   !PvNode
      &&  depth >= 5 * ONE_PLY
      &&  abs(beta) < VALUE_MATE_IN_MAX_PLY) {

    Value rbeta = min(beta + 200, VALUE_INFINITE);
    Depth rdepth = depth - 4 * ONE_PLY;

    assert(rdepth >= ONE_PLY);
    assert((ss-1)->currentMove != MOVE_NONE);
    assert((ss-1)->currentMove != MOVE_NULL);

    MovePicker mp;
    mp_init_pc(&mp, pos, ttMove, PieceValue[MG][captured_piece_type()]);
    CheckInfo ci;
    checkinfo_init(&ci, pos);

    while ((move = next_move(&mp)) != MOVE_NONE)
      if (is_legal(pos, move, ci.pinned)) {
        ss->currentMove = move;
        ss->counterMoves = &CounterMoveHistory[moved_piece(move)][to_sq(move)];
        do_move(pos, move, &st, gives_check(pos, move, &ci));
        value = -search_NonPV(pos, ss+1, -rbeta, -rbeta+1, rdepth, !cutNode);
        undo_move(pos, move);
        if (value >= rbeta)
          return value;
      }
  }

  // Step 10. Internal iterative deepening (skipped when in check)
  if (    depth >= (PvNode ? 5 * ONE_PLY : 8 * ONE_PLY)
      && !ttMove
      && (PvNode || ss->staticEval + 256 >= beta)) {

    Depth d = depth - 2 * ONE_PLY - (PvNode ? DEPTH_ZERO : depth / 4);
    ss->skipEarlyPruning = 1;
    func(search)(pos, ss, alpha, beta, d, cutNode);
    ss->skipEarlyPruning = 0;

    tte = tt_probe(posKey, &ttHit);
    ttMove = ttHit ? tte_move(tte) : 0;
  }

moves_loop: // When in check search starts from here.
  ;  // Avoid a compiler warning. A label must be followed by a statement.
  CounterMoveStats* cmh  = (ss-1)->counterMoves;
  CounterMoveStats* fmh  = (ss-2)->counterMoves;
  CounterMoveStats* fmh2 = (ss-4)->counterMoves;

  MovePicker mp;
  mp_init(&mp, pos, ttMove, depth, ss);
  CheckInfo ci;
  checkinfo_init(&ci, pos);
  value = bestValue; // Workaround a bogus 'uninitialized' warning under gcc
  improving =   ss->staticEval >= (ss-2)->staticEval
          /* || ss->staticEval == VALUE_NONE Already implicit in the previous condition */
             ||(ss-2)->staticEval == VALUE_NONE;

  singularExtensionNode =   !rootNode
                         &&  depth >= 8 * ONE_PLY
                         &&  ttMove != MOVE_NONE
                     /*  &&  ttValue != VALUE_NONE Already implicit in the next condition */
                         &&  abs(ttValue) < VALUE_KNOWN_WIN
                         && !excludedMove // Recursive singular search is not allowed
                         && (tte_bound(tte) & BOUND_LOWER)
                         &&  tte_depth(tte) >= depth - 3 * ONE_PLY;

  // Step 11. Loop through moves
  // Loop through all pseudo-legal moves until no moves remain or a beta cutoff occurs
  while ((move = next_move(&mp)) != MOVE_NONE) {
    assert(move_is_ok(move));

    if (move == excludedMove)
      continue;

    // At root obey the "searchmoves" option and skip moves not listed in Root
    // Move List. As a consequence any illegal move is also skipped. In MultiPV
    // mode we also skip PV moves which have been already searched.
    if (rootNode) {
      size_t idx;
      for (idx = 0; idx < thisThread->rootMoves->size; idx++)
        if (thisThread->rootMoves->move[idx].pv[0] == move)
          break;
      if (idx < thisThread->rootMoves->size)
        continue;
    }

    ss->moveCount = ++moveCount;

    if (rootNode && thisThread->idx == 0 && time_elapsed() > 3000) {
      char buf[16];
      IO_LOCK;
      printf("info depth %d currmove %s currmovenumber %"PRIu64"\n",
             depth / ONE_PLY,
             uci_move(buf, move, is_chess960()),
             moveCount + thisThread->PVIdx);
      IO_UNLOCK;
    }

    if (PvNode)
      (ss+1)->pv = NULL;

    extension = DEPTH_ZERO;
    captureOrPromotion = is_capture_or_promotion(pos, move);
    moved_piece = moved_piece(move);

    givesCheck =  type_of_m(move) == NORMAL && !ci.dcCandidates
               ? (int)(ci.checkSquares[type_of_p(moved_piece(move))] & sq_bb(to_sq(move)))
               : gives_check(pos, move, &ci);

    moveCountPruning =   depth < 16 * ONE_PLY
                      && moveCount >= FutilityMoveCounts[improving][depth];

    // Step 12. Extend checks
    if (    givesCheck
        && !moveCountPruning
        &&  see_sign(pos, move) >= VALUE_ZERO)
      extension = ONE_PLY;

    // Singular extension search. If all moves but one fail low on a search of
    // (alpha-s, beta-s), and just one fails high on (alpha, beta), then that move
    // is singular and should be extended. To verify this we do a reduced search
    // on all the other moves but the ttMove and if the result is lower than
    // ttValue minus a margin then we extend the ttMove.
    if (    singularExtensionNode
        &&  move == ttMove
        && !extension
        &&  is_legal(pos, move, ci.pinned)) {

      Value rBeta = ttValue - 2 * depth / ONE_PLY;
      ss->excludedMove = move;
      ss->skipEarlyPruning = 1;
      value = search_NonPV(pos, ss, rBeta - 1, rBeta, depth / 2, cutNode);
      ss->skipEarlyPruning = 0;
      ss->excludedMove = MOVE_NONE;

      if (value < rBeta)
        extension = ONE_PLY;
    }

    // Update the current move (this must be done after singular extension search)
    newDepth = depth - ONE_PLY + extension;

    // Step 13. Pruning at shallow depth
    if (   !rootNode
        && !captureOrPromotion
        && !inCheck
        && !givesCheck
        && !advanced_pawn_push(pos, move)
        &&  bestValue > VALUE_MATED_IN_MAX_PLY) {

      // Move count based pruning
      if (moveCountPruning)
        continue;

      // Countermoves based pruning
      if (   depth <= 4 * ONE_PLY
          && move != ss->killers[0]
          && (!cmh  || (*cmh )[moved_piece][to_sq(move)] < VALUE_ZERO)
          && (!fmh  || (*fmh )[moved_piece][to_sq(move)] < VALUE_ZERO)
          && (!fmh2 || (*fmh2)[moved_piece][to_sq(move)] < VALUE_ZERO || (cmh && fmh)))
        continue;

      predictedDepth = max(newDepth - func(reduction)(improving, depth, moveCount), DEPTH_ZERO);

      // Futility pruning: parent node
      if (   predictedDepth < 7 * ONE_PLY
          && ss->staticEval + futility_margin(predictedDepth) + 256 <= alpha)
        continue;

      // Prune moves with negative SEE at low depths
      if (predictedDepth < 4 * ONE_PLY && see_sign(pos, move) < VALUE_ZERO)
        continue;
    }

    // Speculative prefetch as early as possible
    prefetch(tt_first_entry(key_after(pos, move)));

    // Check for legality just before making the move
    if (!rootNode && !is_legal(pos, move, ci.pinned)) {
      ss->moveCount = --moveCount;
      continue;
    }

    ss->currentMove = move;
    ss->counterMoves = &CounterMoveHistory[moved_piece][to_sq(move)];

    // Step 14. Make the move
    do_move(pos, move, &st, givesCheck);

    // Step 15. Reduced depth search (LMR). If the move fails high it will be
    // re-searched at full depth.
    if (    depth >= 3 * ONE_PLY
        &&  moveCount > 1
        && !captureOrPromotion) {

      Depth r = func(reduction)(improving, depth, moveCount);
      Value val = (*thisThread->history)[moved_piece][to_sq(move)]
                 +    (cmh  ? (*cmh )[moved_piece][to_sq(move)] : 0)
                 +    (fmh  ? (*fmh )[moved_piece][to_sq(move)] : 0)
                 +    (fmh2 ? (*fmh2)[moved_piece][to_sq(move)] : 0);

      // Increase reduction for cut nodes
      if (cutNode)
        r += 2 * ONE_PLY;

      // Decrease reduction for moves that escape a capture. Filter out
      // castling moves, because they are coded as "king captures rook" and
      // hence break make_move(). Also use see() instead of see_sign(),
      // because the destination square is empty.
      else if (   type_of_m(move) == NORMAL
               && type_of_p(piece_on(to_sq(move))) != PAWN
               && see(pos, make_move(to_sq(move), from_sq(move))) < VALUE_ZERO)
        r -= 2 * ONE_PLY;

      // Decrease/increase reduction for moves with a good/bad history
      int rHist = (val - 10000) / 20000;
      r = max(DEPTH_ZERO, r - rHist * ONE_PLY);

      Depth d = max(newDepth - r, ONE_PLY);

      value = -search_NonPV(pos, ss+1, -(alpha+1), -alpha, d, 1);

      doFullDepthSearch = (value > alpha && r != DEPTH_ZERO);

    } else
      doFullDepthSearch = !PvNode || moveCount > 1;

    // Step 16. Full depth search when LMR is skipped or fails high
    if (doFullDepthSearch)
        value = newDepth <   ONE_PLY ?
                          givesCheck ? -qsearch_NonPV_true(pos, ss+1, -(alpha+1), -alpha, DEPTH_ZERO)
                                     : -qsearch_NonPV_false(pos, ss+1, -(alpha+1), -alpha, DEPTH_ZERO)
                                     : - search_NonPV(pos, ss+1, -(alpha+1), -alpha, newDepth, !cutNode);

    // For PV nodes only, do a full PV search on the first move or after a fail
    // high (in the latter case search only if value < beta), otherwise let the
    // parent node fail low with value <= alpha and try another move.
    if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta)))) {
      (ss+1)->pv = pv;
      (ss+1)->pv[0] = MOVE_NONE;

      value = newDepth <   ONE_PLY ?
                        givesCheck ? -qsearch_PV_true(pos, ss+1, -beta, -alpha, DEPTH_ZERO)
                                   : -qsearch_PV_false(pos, ss+1, -beta, -alpha, DEPTH_ZERO)
                                   : - search_PV(pos, ss+1, -beta, -alpha, newDepth, 0);
    }

    // Step 17. Undo move
    undo_move(pos, move);

    assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

    // Step 18. Check for a new best move
    // Finished searching the move. If a stop occurred, the return value of
    // the search cannot be trusted, and we return immediately without
    // updating best move, PV and TT.
    if (load_rlx(Signals.stop))
      return 0;

    if (rootNode) {
      RootMove *rm = NULL;
      for (size_t idx = 0; idx < thisThread->rootMoves->size; idx++)
        if (thisThread->rootMoves->move[idx].pv[0] == move) {
          rm = &thisThread->rootMoves->move[idx];
          break;
        }

      // PV move or new best move ?
      if (moveCount == 1 || value > alpha) {
        rm->score = value;
        rm->pv_size = 1;

        assert((ss+1)->pv);

        for (Move *m = (ss+1)->pv; *m != MOVE_NONE; ++m)
          rm->pv[rm->pv_size++] = *m;

        // We record how often the best move has been changed in each
        // iteration. This information is used for time management: When
        // the best move changes frequently, we allocate some more time.
        if (moveCount > 1 && thisThread->idx == 0)
          mainThread.bestMoveChanges++;
      } else
        // All other moves but the PV are set to the lowest value: this is
        // not a problem when sorting because the sort is stable and the
        // move position in the list is preserved - just the PV is pushed up.
        rm->score = -VALUE_INFINITE;
    }

    if (value > bestValue) {
      bestValue = value;

      if (value > alpha) {
        // If there is an easy move for this position, clear it if unstable
        if (    PvNode
            &&  thisThread == threads_main()
            &&  easy_move_get(pos_key())
            && (move != easy_move_get(pos_key()) || moveCount > 1))
          easy_move_clear();

        bestMove = move;

        if (PvNode && !rootNode) // Update pv even in fail-high case
          update_pv(ss->pv, move, (ss+1)->pv);

        if (PvNode && value < beta) // Update alpha! Always alpha < beta
          alpha = value;
        else {
          assert(value >= beta); // Fail high
          break;
        }
      }
    }

    if (!captureOrPromotion && move != bestMove && quietCount < 64)
      quietsSearched[quietCount++] = move;
  }

  // The following condition would detect a stop only after move loop has been
  // completed. But in this case bestValue is valid because we have fully
  // searched our subtree, and we can anyhow save the result in TT.
  /*
  if (Signals.stop)
    return VALUE_DRAW;
  */

  // Step 20. Check for mate and stalemate
  // All legal moves have been searched and if there are no legal moves, it
  // must be a mate or a stalemate. If we are in a singular extension search then
  // return a fail low score.
  if (!moveCount)
    bestValue = excludedMove ? alpha
               :     inCheck ? mated_in(ss->ply) : DrawValue[pos_stm()];

  // Quiet best move: update killers, history and countermoves
  else if (bestMove && !is_capture_or_promotion(pos, bestMove))
    update_stats(pos, ss, bestMove, depth, quietsSearched, quietCount);

  // Bonus for prior countermove that caused the fail low
  else if (    depth >= 3 * ONE_PLY
           && !bestMove
           && !captured_piece_type()
           && move_is_ok((ss-1)->currentMove)) {
    Square prevSq = to_sq((ss-1)->currentMove);
    Value bonus = (Value)((depth / ONE_PLY) * (depth / ONE_PLY) + 2 * depth / ONE_PLY - 2);
    if ((ss-2)->counterMoves)
      cms_update(*(ss-2)->counterMoves, piece_on(prevSq), prevSq, bonus);

    if ((ss-3)->counterMoves)
      cms_update(*(ss-3)->counterMoves, piece_on(prevSq), prevSq, bonus);

    if ((ss-5)->counterMoves)
      cms_update(*(ss-5)->counterMoves, piece_on(prevSq), prevSq, bonus);
  }

  tte_save(tte, posKey, value_to_tt(bestValue, ss->ply),
           bestValue >= beta ? BOUND_LOWER :
           PvNode && bestMove ? BOUND_EXACT : BOUND_UPPER,
           depth, bestMove, ss->staticEval, tt_generation());

  assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

  return bestValue;
}

#undef PvNode
#undef func
