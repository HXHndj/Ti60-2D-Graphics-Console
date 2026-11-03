/* Independent post-Boss altar sequence. Time is measured in simulation ticks,
 * not CPU render time. No new hardware opcode or source allocation is needed. */
#ifndef MC_SHRINE_RITUAL_H
#define MC_SHRINE_RITUAL_H
#define MC_RITUAL_CHARGE_TICKS 36u
#define MC_RITUAL_IGNITION_END 48u
#define MC_RITUAL_MAX_SPRITES 192u
#define MC_RITUAL_CAST_INTERVAL 42u
#define MC_RITUAL_FINISH_TICKS 96u
#define MC_RITUAL_FINISH_IMPACT 24u
enum mc_ritual_state {
 MC_RITUAL_NONE=0,MC_RITUAL_READY,MC_RITUAL_CHARGING,
 MC_RITUAL_RUNNING,MC_RITUAL_DONE
};
#endif
