package main

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"os"
	"strings"

	"github.com/aws/aws-sdk-go-v2/aws"
	awsconfig "github.com/aws/aws-sdk-go-v2/config"
	"github.com/aws/aws-sdk-go-v2/service/ses"
	"github.com/aws/aws-sdk-go-v2/service/ses/types"
)

type awsSES interface {
	SendBulkTemplatedEmail(ctx context.Context, params *ses.SendBulkTemplatedEmailInput, optFns ...func(*ses.Options)) (*ses.SendBulkTemplatedEmailOutput, error)
}

const (
	fieldsPerRecord           = 5
	fromEmailActivation       = "Mercury <noreply@example.com>"
	templateActivation        = "ActivationEmail"
	configSetActivation       = "default"
	fromEmailPasswordRecovery = "Mercury <noreply@example.com>"
	templatePasswordRecovery  = "PasswordRecoveryEmail"
	configSetPasswordRecovery = "default"
)

var devMode = flag.Bool("debug", false, "enable fake SES client")

func main() {
	flag.Parse()

	var c awsSES
	if !*devMode {
		awscfg, err := awsconfig.LoadDefaultConfig(context.Background())
		if err != nil {
			fmt.Fprintf(os.Stderr, "[FATAL] failed to load AWS default config: %v\n", err)
			os.Exit(1)
		}
		c = ses.NewFromConfig(awscfg)
	} else {
		fmt.Fprint(os.Stderr, "[INFO] debug mode enabled; mock SES client is being used\n")
		c = &mockSESClient{}
	}

	scanner := bufio.NewScanner(os.Stdin)

	for scanner.Scan() {
		line := scanner.Text()
		if line == "" {
			continue
		}

		destinationsActivation, destinationsPasswordRecovery, err := parseDestinations(line)
		if err != nil {
			fmt.Fprintf(os.Stderr, "[ERROR] %v\n", err)
			continue
		}

		if len(destinationsActivation) > 0 {
			sendBulkEmails(c, fromEmailActivation, templateActivation, configSetActivation, destinationsActivation, "{\"login\":\"\",\"secret\":\"\"}")
		}

		if len(destinationsPasswordRecovery) > 0 {
			sendBulkEmails(c, fromEmailPasswordRecovery, templatePasswordRecovery, configSetPasswordRecovery, destinationsPasswordRecovery, "{\"login\":\"\",\"secret\":\"\",\"code\":\"\"}")
		}
	}

	if err := scanner.Err(); err != nil {
		fmt.Fprintf(os.Stderr, "failed to read from STDIN: %v\n", err)
	}
}

func parseDestinations(line string) ([]types.BulkEmailDestination, []types.BulkEmailDestination, error) {
	fields := strings.Split(line, ",")
	if len(fields)%fieldsPerRecord != 0 {
		return nil, nil, fmt.Errorf("invalid input format; line doesn't align with %d fields per record", fieldsPerRecord)
	}

	var destinationsActivation []types.BulkEmailDestination
	var destinationsPasswordRecovery []types.BulkEmailDestination

	for i := 0; i < len(fields); i += fieldsPerRecord {
		action := fields[i]
		email := fields[i+1]
		username := fields[i+2]
		secret := fields[i+3]
		code := fields[i+4]

		switch action {
		case "activation":
			destinationsActivation = append(destinationsActivation, types.BulkEmailDestination{
				Destination: &types.Destination{
					ToAddresses: []string{email},
				},
				ReplacementTemplateData: aws.String(fmt.Sprintf("{\"login\":\"%s\",\"secret\":\"%s\"}", username, secret)),
			})
		case "password_recovery":
			destinationsPasswordRecovery = append(destinationsPasswordRecovery, types.BulkEmailDestination{
				Destination: &types.Destination{
					ToAddresses: []string{email},
				},
				ReplacementTemplateData: aws.String(fmt.Sprintf("{\"login\":\"%s\",\"secret\":\"%s\",\"code\":\"%s\"}", username, secret, code)),
			})
		default:
			fmt.Fprintf(os.Stderr, "[WARN] unknown action %q, skipping destination...\n", action)
		}
	}

	return destinationsActivation, destinationsPasswordRecovery, nil
}

func sendBulkEmails(client awsSES, fromEmail, template, configSet string, destinations []types.BulkEmailDestination, defaultTemplateData string) {
	input := &ses.SendBulkTemplatedEmailInput{
		Source:               aws.String(fromEmail),
		Template:             aws.String(template),
		ConfigurationSetName: aws.String(configSet),
		DefaultTemplateData:  aws.String(defaultTemplateData),
		Destinations:         destinations,
	}

	ctx := context.Background()
	output, err := client.SendBulkTemplatedEmail(ctx, input)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[ERROR] failed to send bulk templated email: %v\n", err)
		return
	}

	fmt.Fprintf(os.Stderr, "[INFO] sent batch with %d destinations using template: %s\n", len(destinations), template)
	for i, status := range output.Status {
		recipient := input.Destinations[i].Destination.ToAddresses[0]
		fmt.Fprintf(os.Stderr, "Recipient: %s, Status: %s\n", recipient, status.Status)
	}
}

type mockSESClient struct{}

func (m *mockSESClient) SendBulkTemplatedEmail(ctx context.Context, params *ses.SendBulkTemplatedEmailInput, optFns ...func(*ses.Options)) (*ses.SendBulkTemplatedEmailOutput, error) {
	fmt.Println("[DEBUG] Mock SendBulkTemplatedEmail called")
	for _, dest := range params.Destinations {
		fmt.Printf("[DEBUG] To: %v, TemplateData: %v\n", dest.Destination.ToAddresses, dest.ReplacementTemplateData)
	}
	statuses := make([]types.BulkEmailDestinationStatus, len(params.Destinations))
	for i := range statuses {
		statuses[i] = types.BulkEmailDestinationStatus{Status: "Success"}
	}
	return &ses.SendBulkTemplatedEmailOutput{Status: statuses}, nil
}
