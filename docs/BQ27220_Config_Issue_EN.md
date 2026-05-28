# BQ27220 Parameter Configuration Issue

## Goal

Configure BQ27220 fuel gauge for a 1500mAh battery (default is 3000mAh).

## What We Tried

### 1. Write Data Memory RAM

Addresses: 0x929F (Design Capacity), 0x929D (Full Charge Capacity), etc.

- Write operation shows success
- Immediate verification after write shows 1500mAh
- After exiting Config Update mode, Command 0x3C and FCC still show 3000mAh
- RM value exceeds 1500mAh, indicating SOC calculation uses 3000mAh default

### 2. Write All CEDV Parameters

Parameters: EMF, R0, R1, C0, C1, T0, TC, EDV thresholds, voltage-DOD curve

- Same result: RAM write succeeds, but doesn't affect actual behavior

### 3. Tried Different Approaches

- Used library's `writeDataMemoryU16()` function
- Used raw I2C commands with proper checksum calculation (replacement method from TRM page 65)
- Verified Config Update mode is entered (OperationStatus bit2 = 1)

## Key Observations

1. **RAM write succeeds** - immediate read-back after write shows correct values
2. **Command registers read NVM** - 0x3C (DesignCapacity) always returns 3000mAh after exit Config Update
3. **SOC calculation uses NVM** - RM = 2473mAh (82% of 3000mAh), not based on 1500mAh
4. **According to TRM Section 8.1**: "These updates are stored in RAM and need to be re-programmed any time the device loses power"
5. **Even with all parameters written**, behavior doesn't change

## Questions

1. What is the correct way to configure BQ27220 for a different battery capacity via I2C only (without EV2300/EV2400)?

2. Is it possible to make RAM-written parameters take effect for SOC calculation? If yes, what is the correct sequence?

3. Does BQ27220 support runtime parameter modification via I2C, or is OTP programming the only way?

4. What is the purpose of RAM-based Data Memory writes if they don't affect the gauge's behavior?

5. Is there a way to generate gm.fs files without BQStudio/EV2300, or an alternative method to configure BQ27220?

## Environment

- BQ27220YZFR
- ESP32 (Arduino framework)
- I2C communication at 100kHz
- No EV2300/EV2400 available

## References

- BQ27220 Datasheet (SLUSCB7)
- BQ27220 Technical Reference Manual (SLUUBD4A)
