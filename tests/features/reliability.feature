@reliability
Feature: Reliability and Fault Tolerance
  Ensure graceful handling of node failures, partitions, and overload

  Scenario: Graceful node failure handling
    Given the UCK module is loaded
    When a remote node dies
    Then processes accessing dead node pages should receive SIGSEGV
    And the load balancer should stop targeting the dead node

  Scenario: Network partition detection with quorum
    Given the UCK module is loaded
    When a 3-node cluster has a network partition isolating 1 node
    Then the isolated node should enter degraded mode

  Scenario: Fencing prevents stale node conflicts
    Given the UCK module is loaded
    When a dead node returns to the cluster
    Then it must re-join with the current epoch
    And stale messages from the old epoch are rejected

  Scenario: Page fetch retry with timeout
    Given the UCK module is loaded
    When a page owner is slow to respond
    Then the fetch should retry 3 times
    And on final failure return SIGSEGV

  Scenario: Adaptive heartbeat failure detection
    Given the UCK module is loaded
    When one heartbeat is delayed
    Then the node should remain marked as alive
    When three consecutive heartbeats are missed
    Then the node should be marked as dead

  Scenario: Resource limits prevent exhaustion
    Given the UCK module is loaded
    When a node is at maximum task capacity
    Then new fork distribution should keep the process local
