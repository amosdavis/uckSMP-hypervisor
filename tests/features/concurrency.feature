@concurrency
Feature: Concurrency and Correctness
  Ensure no race conditions, deadlocks, or memory corruption

  Scenario: Concurrent page faults on same page
    Given the UCK module is loaded
    When multiple threads fault on the same page simultaneously
    Then exactly one network fetch should occur
    And all threads should see correct data

  Scenario: Lock ordering under concurrent operations
    Given the UCK module is loaded
    When concurrent operations run under lockdep
    Then no lockdep warnings should be generated

  Scenario: Atomic page state transitions
    Given the UCK module is loaded
    When concurrent read and write faults occur on the same page
    Then all final page states should be valid

  Scenario: Migration concurrency safety
    Given the UCK module is loaded
    When a process is migrated while pages are being faulted
    Then no memory corruption should occur

  Scenario: Futex under network partition
    Given the UCK module is loaded
    When a futex wake is sent to a partitioned node
    Then the wake should timeout and notify the application

  Scenario: No use-after-free under stress
    Given the UCK module is loaded
    When a KASAN-enabled build runs stress tests
    Then no KASAN violations should be reported
