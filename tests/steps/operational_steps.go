package steps

import (
	"github.com/cucumber/godog"
)

func initOperationalSteps(ctx *godog.ScenarioContext) {
	ctx.Step(`^invalid configuration is provided$`, invalidConfig)
	ctx.Step(`^a clear error message should be displayed$`, clearErrorMessage)
	ctx.Step(`^the exit code should be non-zero$`, nonZeroExitCode)
	ctx.Step(`^the heartbeat interval is changed at runtime$`, heartbeatIntervalChanged)
	ctx.Step(`^the new interval should take effect without module reload$`, newIntervalTakesEffect)
	ctx.Step(`^cluster operations have been performed$`, clusterOpsPerformed)
	ctx.Step(`^all procfs files should be readable$`, procfsReadable)
	ctx.Step(`^all procfs files should contain expected data$`, procfsContainExpected)
	ctx.Step(`^the module is unloaded with active processes$`, moduleUnloadedWithProcs)
	ctx.Step(`^processes should be migrated to other nodes$`, procsMigrated)
	ctx.Step(`^peers should be notified$`, peersNotified)
}

func invalidConfig() error            { return godog.ErrPending }
func clearErrorMessage() error        { return godog.ErrPending }
func nonZeroExitCode() error          { return godog.ErrPending }
func heartbeatIntervalChanged() error { return godog.ErrPending }
func newIntervalTakesEffect() error   { return godog.ErrPending }
func clusterOpsPerformed() error      { return godog.ErrPending }
func procfsReadable() error           { return godog.ErrPending }
func procfsContainExpected() error    { return godog.ErrPending }
func moduleUnloadedWithProcs() error  { return godog.ErrPending }
func procsMigrated() error            { return godog.ErrPending }
func peersNotified() error            { return godog.ErrPending }
