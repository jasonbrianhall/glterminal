#define EXTERN
#define INIT
#include "../zork/funcs.h"
#include "../zork/vars.h"

// Called by wopr_zork.cpp's thread instead of main()
int zork_main(void)
{
    if (init_())
        game_();
    return 0;
}
