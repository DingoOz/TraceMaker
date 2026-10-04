#pragma once
// Name conventions used to recognise parts and nets (doc 15 §3.1): reference letters, power and ground net
// names, natural sort of references, component values. Pure string functions, no board access.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tmk::crules {

std::string upper(std::string_view s);
std::string lower(std::string_view s);
// Leading letters of a reference, upper case: "C12" -> "C", "IC3" -> "IC", "FB1" -> "FB", "REF**" -> "REF".
std::string ref_letters(std::string_view ref);
// Natural order of references: "C2" < "C10" < "D1"; ties broken by plain string comparison.
bool natural_less(std::string_view a, std::string_view b);
// The last hierarchical component of a net name: "/mcu/XTAL_IN" -> "XTAL_IN".
std::string_view net_leaf(std::string_view name);

// Supply-like net names (+3V3, VCC, VDD_IO, 5V, VBUS, GND, ...), including grounds. Decision D25's test, shared
// with the placer (place::power_like_name forwards here).
bool power_like_name(std::string_view name);
bool ground_like_name(std::string_view name);
// Analog supplies, references and core-regulator outputs that power_like_name misses (VREF, AREF, VCAP, ...).
bool analog_supply_name(std::string_view name);

// Voltage in millivolts from a supply name: "+5V" 5000, "3V3" 3300, "+3.3V" 3300, "1V8" 1800, "VCC_12V" 12000.
std::optional<int> net_millivolts(std::string_view name);
// Capacitance in picofarads from a value field: "100nF" 100000, "4u7" 4700000, "10uF/16V" 10000000, "22p" 22.
std::optional<std::int64_t> capacitance_pf(std::string_view value);

}  // namespace tmk::crules
