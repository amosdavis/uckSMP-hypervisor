package steps

import (
	"github.com/cucumber/godog"
)

func initReliabilitySteps(ctx *godog.ScenarioContext) {
	ctx.Step(`^a remote node dies$`, remoteNodeDies)
	ctx.Step(`^processes accessing dead node pages should receive SIGSEGV$`, processesSIGSEGV)
	ctx.Step(`^the load balancer should stop targeting the dead node$`, loadBalancerStopsDeadNode)
	ctx.Step(`^a (\d+)-node cluster has a network partition isolating (\d+) node$`, networkPartition)
	ctx.Step(`^the isolated node should enter degraded mode$`, isolatedNodeDegraded)
	ctx.Step(`^a dead node returns to the cluster$`, deadNodeReturns)
	ctx.Step(`^it must re-join with the current epoch$`, mustReJoinWithEpoch)
	ctx.Step(`^stale messages from the old epoch are rejected$`, staleMessagesRejected)
	ctx.Step(`^a page owner is slow to respond$`, pageOwnerSlowToRespond)
	ctx.Step(`^the fetch should retry (\d+) times$`, fetchShouldRetry)
	ctx.Step(`^on final failure return SIGSEGV$`, finalFailureSIGSEGV)
	ctx.Step(`^one heartbeat is delayed$`, oneHeartbeatDelayed)
	ctx.Step(`^the node should remain marked as alive$`, nodeRemainsAlive)
	ctx.Step(`^three consecutive heartbeats are missed$`, threeConsecutiveMissed)
	ctx.Step(`^the node should be marked as dead$`, nodeMarkedDead)
	ctx.Step(`^a node is at maximum task capacity$`, nodeAtMaxTasks)
	ctx.Step(`^new fork distribution should keep the process local$`, forkKeepsLocal)
}

func remoteNodeDies() error                          { return godog.ErrPending }
func processesSIGSEGV() error                        { return godog.ErrPending }
func loadBalancerStopsDeadNode() error               { return godog.ErrPending }
func networkPartition(total, isolated int) error      { return godog.ErrPending }
func isolatedNodeDegraded() error                    { return godog.ErrPending }
func deadNodeReturns() error                         { return godog.ErrPending }
func mustReJoinWithEpoch() error                     { return godog.ErrPending }
func staleMessagesRejected() error                   { return godog.ErrPending }
func pageOwnerSlowToRespond() error                  { return godog.ErrPending }
func fetchShouldRetry(n int) error                   { return godog.ErrPending }
func finalFailureSIGSEGV() error                     { return godog.ErrPending }
func oneHeartbeatDelayed() error                     { return godog.ErrPending }
func nodeRemainsAlive() error                        { return godog.ErrPending }
func threeConsecutiveMissed() error                  { return godog.ErrPending }
func nodeMarkedDead() error                          { return godog.ErrPending }
func nodeAtMaxTasks() error                          { return godog.ErrPending }
func forkKeepsLocal() error                          { return godog.ErrPending }
