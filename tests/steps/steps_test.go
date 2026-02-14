package steps

import (
	"testing"

	"github.com/cucumber/godog"
)

func TestFeatures(t *testing.T) {
	suite := godog.TestSuite{
		ScenarioInitializer: InitializeScenario,
		Options: &godog.Options{
			Format:   "pretty",
			Paths:    []string{"../features"},
			TestingT: t,
		},
	}

	if suite.Run() != 0 {
		t.Fatal("non-zero status returned, failed to run feature tests")
	}
}

func InitializeScenario(ctx *godog.ScenarioContext) {
	// Security steps
	initSecuritySteps(ctx)
	// Concurrency steps
	initConcurrencySteps(ctx)
	// Reliability steps
	initReliabilitySteps(ctx)
	// Memory steps
	initMemorySteps(ctx)
	// Migration steps
	initMigrationSteps(ctx)
	// Networking steps
	initNetworkingSteps(ctx)
	// Scheduling steps
	initSchedulingSteps(ctx)
	// Operational steps
	initOperationalSteps(ctx)
	// Architecture steps
	initArchitectureSteps(ctx)
}
