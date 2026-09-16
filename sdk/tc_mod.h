#ifndef TC_MOD_H
#define TC_MOD_H

#include "tc_mod_api.h"
#include "tc_game_model.h"
#include "tc_board_model.h"
#include "tc_game_state.h"
#include "tc_simulation.h"
#include "tc_wire_model.h"
#include "tc_save_model.h"

namespace tc {

struct TCMod {
    TCGameModel game;
    TCBoardModel board;
    TCGameStateModel state;
    TCSimulationModel simulation;
    TCWireModel wire;
    TCSaveModel save;

    bool load(const TCHost* host) {
        return game.load(host) && board.load(host) && state.load(host) &&
               simulation.load(host) && wire.load(host) && save.load(host);
    }

    bool valid() const {
        return game.valid() && board.valid() && state.valid() &&
               simulation.valid() && wire.valid() && save.valid();
    }
};

}  // namespace tc

#endif  // TC_MOD_H
