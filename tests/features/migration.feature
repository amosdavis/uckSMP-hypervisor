@migration
Feature: Process Migration Hardening
  Ensure safe, correct, and complete process migration

  Scenario: Multi-threaded process migration
    Given the UCK module is loaded
    When a 4-thread process is migrated
    Then all threads should resume on the destination

  Scenario: Migration convergence for write-heavy processes
    Given the UCK module is loaded
    When a write-heavy process is being migrated
    Then migration should converge within configured iterations

  Scenario: CPU feature compatibility check
    Given the UCK module is loaded
    When the destination node lacks AVX support
    Then migration should be rejected with a clear error

  Scenario: Atomic migration with network failure
    Given the UCK module is loaded
    When the network drops mid-migration
    Then the destination should clean up staging state
    And the source process should be resumed

  Scenario: Post-migration CRC validation
    Given the UCK module is loaded
    When a page is intentionally corrupted during transfer
    Then the CRC mismatch should be detected
    And migration should be aborted
