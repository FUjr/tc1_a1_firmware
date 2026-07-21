#ifndef __USER_POWER_H_
#define __USER_POWER_H_

#define PW_NUM 100
#define POWER_COEFFICIENT_SCALE 100
#define POWER_COEFFICIENT_BL0937_X100 1710
#define POWER_COEFFICIENT_BL0937B_X100 1395
#define POWER_COEFFICIENT_MIN_X100 100
#define POWER_COEFFICIENT_MAX_X100 10000

typedef struct
{
    int idx;
    uint32_t powers[PW_NUM];
} PowerRecord;

extern PowerRecord power_record;
extern uint32_t p_count;
extern float real_time_power;

char* GetPowerRecord(int idx);
float GetPowerCoefficient(void);
float PowerPulseCountToKwh(uint32_t pulse_count);
void PowerInit(void);
void SetPowerRecord(PowerRecord* pr, uint32_t pw);

#endif
