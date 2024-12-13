# mercury

This project comprises a system for harvesting user action tokens from a PostgreSQL database and sending bulk templated emails using AWS Simple Email Service (SES). The primary components of the project include:

1. `token_harvester.c`: A C program that listens to a PostgreSQL notification channel, fetches token data, and outputs it in batches.
2. `send_bulk_templated_email.sh`: A Bash script that processes the output of `token_harvester.c` and sends emails in bulk.
3. `mock_aws_ses.sh`: A mock script for testing AWS SES email sending.
4. Migrations for database setup and initialization.

## Table of Contents

- [Requirements](#requirements)
- [Environment Variables](#environment-variables)
- [Components](#components)
  - [Token Harvester](#token-harvester)
  - [Email Processor Script](#email-processor-script)
  - [Mock AWS SES Script](#mock-aws-ses-script)
  - [Database Migrations](#database-migrations)
  - [Build and Deployment](#build-and-deployment)
- [Usage](#usage)
- [Testing](#testing)

## Requirements

- PostgreSQL server with appropriate schema and data.
- AWS CLI installed and configured for SES.
- Bash environment.
- JSON processing tools (`jq`).
- `go-migrate` for managing database migrations.

## Environment Variables

### Token Harvester (`token_harvester.c`)

| Variable        | Default Value                          | Description                                                                 |
|-----------------|----------------------------------------|-----------------------------------------------------------------------------|
| `DB_CONNSTR`    | `host=localhost user=postgres dbname=postgres` | PostgreSQL connection string.                                               |
| `CHANNEL_NAME`  | `token_insert`                        | PostgreSQL channel to listen for notifications.                             |
| `EVENT_THRESHOLD` | `10`                                  | Number of notifications to accumulate before fetching actions.              |
| `TIMEOUT_MS`    | `5000`                                | Timeout in milliseconds for waiting on PostgreSQL notifications.            |

### Email Processor Script (`send_bulk_templated_email.sh`)

| Variable             | Default Value              | Description                                                                |
|----------------------|----------------------------|----------------------------------------------------------------------------|
| `AWS_MOCK_DEBUG`     | `Not set`                 | If set, enables mock AWS CLI for testing email sending.                    |
| `BASE_OUTDIR`        | `data`                    | Base directory for storing email payloads.                                 |

## Components

### Token Harvester

The `token_harvester.c` program:
- Listens to a PostgreSQL notification channel for `token_insert` events.
- Queries the database for relevant token data based on a provided query.
- Outputs the data in CSV format for further processing.

#### Compilation

```bash
make release
```

#### Debug Build

To enable a debug build with address sanitization and debugging symbols:

```bash
make debug
```

#### Execution

```bash
./token_harvester
```

### Email Processor Script

The `send_bulk_templated_email.sh` script:
- Reads token data in CSV format from the `token_harvester` output.
- Groups data by email type (e.g., activation, password recovery).
- Constructs bulk email payloads in JSON format.
- Sends emails using AWS SES.

#### Usage

Run the script by piping the output of `token_harvester`:

```bash
./token_harvester | ./send_bulk_templated_email.sh
```

### Mock AWS SES Script

The `mock_aws_ses.sh` script is a mock replacement for AWS CLI. It:
- Logs the SES command parameters to `mock_aws_ses.log`.
- Outputs a simulated SES response with success and failure examples.

#### Enable Mock Mode

Set the `AWS_MOCK_DEBUG` environment variable:

```bash
export AWS_MOCK_DEBUG=1
```

### Database Migrations

The `migrations` folder contains SQL scripts for initializing and updating the database schema. These scripts are managed using the `go-migrate` tool.

#### Initial Migration

The initial migration includes:
- Creating tables (`accounts`, `tokens`, `jobs`).
- Defining custom types (e.g., `account_status`, `token_action`, `job_type`).
- Adding triggers for token insertion, account status changes, and token consumption.
- Setting up indexes for improved query performance.

To run the migrations:

```bash
go install -tags 'postgres' github.com/golang-migrate/migrate/v4/cmd/migrate@latest

migrate -database "postgres://localhost:5432/example?sslmode=disable" -path migrations up
```

#### Quick Testing

You can quickly populate the database for testing purposes:

```bash
printf "%.0sINSERT INTO accounts (email, login) VALUES ('user' || md5(random()::text) || '@example.com', 'user' || substr(md5(random()::text), 1, 20));select pg_sleep(0.3);\n" {1..38} | psql "postgres://localhost:5432/example"
```

### Build and Deployment

#### Makefile

The provided `Makefile` supports both release and debug builds. It includes platform-specific configurations for macOS and Linux.

- **Key Targets:**
  - `release`: Compiles an optimized binary for production use.
  - `debug`: Compiles a binary with debug symbols and sanitizers for development.
  - `clean`: Removes build artifacts.

#### Dockerfile

The provided `Dockerfile` supports building and deploying the `token_harvester` in a minimal Debian-based container.

1. **Builder Stage:**
   - Installs build tools and dependencies (e.g., `clang`, `libpq-dev`).
   - Compiles the `token_harvester` binary using the `Makefile`.

2. **Runtime Stage:**
   - Installs runtime dependencies (e.g., `libpq5`, `awscli`, `jq`).
   - Copies the compiled binary and scripts from the builder stage.
   - Configures a non-root user and sets `/app` as the working directory.

#### Build Docker Image

```bash
docker build -t token-harvester:latest .
```

#### Run Docker Container

```bash
docker run --rm -v $(pwd)/data:/app/data token-harvester:latest
```

## Usage

1. Start the PostgreSQL server and ensure the required schema is set up.
2. Run the database migrations using `go-migrate`.
3. Set necessary environment variables.
4. Compile and run `token_harvester`.
5. Pipe its output into `send_bulk_templated_email.sh` for bulk email processing.

Example:

```bash
DB_CONNSTR="host=mydbhost user=myuser dbname=mydb" \
CHANNEL_NAME="my_channel" \
EVENT_THRESHOLD=20 \
TIMEOUT_MS=10000 \
./token_harvester | ./send_bulk_templated_email.sh
```

Alternatively, using Docker:

```bash
docker run --rm -e DB_CONNSTR="host=mydbhost user=myuser dbname=mydb" \
  -e CHANNEL_NAME="my_channel" \
  -e EVENT_THRESHOLD=20 \
  -e TIMEOUT_MS=10000 \
  -v $(pwd)/data:/app/data token-harvester:latest
```

## Testing

To test the email processing system:

1. Enable the mock AWS SES script:

    ```bash
    export AWS_MOCK_DEBUG=1
    ```

2. Run the pipeline:

    ```bash
    ./token_harvester | ./send_bulk_templated_email.sh
    ```

3. Inspect the generated payloads in the `data` directory and review `mock_aws_ses.log` for mock SES responses.
