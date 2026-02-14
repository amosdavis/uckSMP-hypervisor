@operational
Feature: Operational Hardening
  Ensure robust configuration, monitoring, and lifecycle management

  Scenario: Configuration validation
    Given the UCK module is loaded
    When invalid configuration is provided
    Then a clear error message should be displayed
    And the exit code should be non-zero

  Scenario: Live parameter updates
    Given the UCK module is loaded
    When the heartbeat interval is changed at runtime
    Then the new interval should take effect without module reload

  Scenario: Comprehensive procfs reporting
    Given the UCK module is loaded
    When cluster operations have been performed
    Then all procfs files should be readable
    And all procfs files should contain expected data

  Scenario: Graceful shutdown with active processes
    Given the UCK module is loaded
    When the module is unloaded with active processes
    Then processes should be migrated to other nodes
    And peers should be notified
