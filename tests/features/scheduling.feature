@scheduling
Feature: Scheduling and Load Balancing
  Ensure fair, efficient, and deadlock-free scheduling

  Scenario: Fair scheduling with heterogeneous nodes
    Given the UCK module is loaded
    And heterogeneous nodes with 4-CPU and 8-CPU
    Then the 8-CPU node should get approximately 2x more forks

  Scenario: Multi-factor load balancing
    Given the UCK module is loaded
    When a node has low CPU but high memory pressure
    Then the load balancer should avoid that node

  Scenario: No soft lockup under high contention
    Given the UCK module is loaded
    When high contention operations run
    Then no soft lockup warnings should appear in dmesg
