package steps

import (
	"github.com/cucumber/godog"
)

func initMigrationSteps(ctx *godog.ScenarioContext) {
	ctx.Step(`^a (\d+)-thread process is migrated$`, multiThreadMigrate)
	ctx.Step(`^all threads should resume on the destination$`, allThreadsResume)
	ctx.Step(`^a write-heavy process is being migrated$`, writeHeavyMigrate)
	ctx.Step(`^migration should converge within configured iterations$`, migrationConverges)
	ctx.Step(`^the destination node lacks AVX support$`, destLacksAVX)
	ctx.Step(`^migration should be rejected with a clear error$`, migrationRejected)
	ctx.Step(`^the network drops mid-migration$`, networkDropsMidMigration)
	ctx.Step(`^the destination should clean up staging state$`, destCleansUpStaging)
	ctx.Step(`^the source process should be resumed$`, sourceProcessResumed)
	ctx.Step(`^a page is intentionally corrupted during transfer$`, pageCorruptedDuringTransfer)
	ctx.Step(`^the CRC mismatch should be detected$`, crcMismatchDetected)
	ctx.Step(`^migration should be aborted$`, migrationAborted)
}

func multiThreadMigrate(threads int) error { return godog.ErrPending }
func allThreadsResume() error              { return godog.ErrPending }
func writeHeavyMigrate() error             { return godog.ErrPending }
func migrationConverges() error            { return godog.ErrPending }
func destLacksAVX() error                  { return godog.ErrPending }
func migrationRejected() error             { return godog.ErrPending }
func networkDropsMidMigration() error      { return godog.ErrPending }
func destCleansUpStaging() error           { return godog.ErrPending }
func sourceProcessResumed() error          { return godog.ErrPending }
func pageCorruptedDuringTransfer() error   { return godog.ErrPending }
func crcMismatchDetected() error           { return godog.ErrPending }
func migrationAborted() error              { return godog.ErrPending }
