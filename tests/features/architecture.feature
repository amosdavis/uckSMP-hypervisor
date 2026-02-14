@architecture
Feature: Architectural Improvements
  Ensure modular design, reduced TCB, and ISA portability

  Scenario: Split architecture preserves functionality
    Given the UCK module is loaded
    When the split architecture is deployed
    Then all existing functionality should work through the split

  Scenario: HAL layer for x86_64
    Given the UCK module is loaded
    When the x86_64 HAL build is used
    Then register capture and restore should work

  Scenario: Modular subsystem loading
    Given the UCK module is loaded
    When only core and heartbeat modules are loaded
    Then reduced functionality should work correctly
