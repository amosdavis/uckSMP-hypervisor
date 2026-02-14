@security
Feature: Security Hardening
  Ensure uckSMP-hypervisor is protected against all known security failure modes

  Scenario: Unauthenticated nodes are rejected
    Given the UCK module is loaded
    And a node is configured with TLS enabled
    When an unauthenticated node attempts to connect
    Then the connection should be rejected
    And network traffic should be encrypted

  Scenario: Heartbeat messages require valid HMAC
    Given the UCK module is loaded
    When a heartbeat message is received without valid HMAC
    Then the heartbeat should be silently dropped

  Scenario: Job execution preserves UID/GID
    Given the UCK module is loaded
    When a process with UID 1000 submits a job
    Then the job should run as UID 1000 on the remote node

  Scenario: Per-region access control
    Given the UCK module is loaded
    When a shared region is created by user "alice"
    And another user "bob" attempts to access the region
    Then access should be denied

  Scenario: Memory is scrubbed on teardown
    Given the UCK module is loaded
    When a region is created and written to
    And the region is torn down
    Then the freed pages should contain only zeros

  Scenario: Migration state files are secure
    Given the UCK module is loaded
    When a process is migrated to a remote node
    Then the migration state file should not be readable by other users
    And the state file should be deleted after restore

  Scenario: Audit logging for security events
    Given the UCK module is loaded
    When an auditable event occurs
    Then an audit record should appear in the audit log

  Scenario: Rate limiting prevents DoS
    Given the UCK module is loaded
    When a flood of page requests is sent from one node
    Then the requests should be throttled

  Scenario: Seccomp filters on restored processes
    Given the UCK module is loaded
    When a restored process attempts to call "ptrace"
    Then the syscall should be blocked by seccomp
