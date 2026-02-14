@memory
Feature: Memory and Page Management
  Ensure correct memory handling, COW, NUMA awareness, and overcommit protection

  Scenario: Copy-on-write correctness across nodes
    Given the UCK module is loaded
    When node 1 writes to a shared page
    And node 2 reads the same page
    Then node 2 should see the updated data

  Scenario: NUMA-aware page placement
    Given the UCK module is loaded
    When pages are allocated on a NUMA system
    Then pages should be allocated on the local NUMA node

  Scenario: Memory overcommit protection
    Given the UCK module is loaded
    When pages are allocated up to the memory limit
    Then further allocations should be rejected cleanly

  Scenario: Adaptive prefetch for sequential access
    Given the UCK module is loaded
    When a sequential access pattern is detected
    Then the prefetch window should increase

  Scenario: Prefetch disabled for random access
    Given the UCK module is loaded
    When a random access pattern is detected
    Then prefetching should be disabled

  Scenario: KSM side-channel mitigation
    Given the UCK module is loaded
    When KSM is enabled on the host
    Then UCK pages should not be merged by KSM
