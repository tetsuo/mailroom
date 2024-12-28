# mailboy

WIP


## Environment Variables

Both components are fully configured using environment variables. Here’s the list, their purposes, and default values:

### listener

| Name                      | Default Value          | Description                                                                |
| ------------------------- | ---------------------- | -------------------------------------------------------------------------- |
| `MB_DATABASE_URL`         | _None (required)_      | PostgreSQL connection string.                                              |
| `MB_SECRET_KEY`           | _None (required)_      | 64-character hexadecimal string used as the secret key for HMAC.           |
| `MB_CHANNEL_NAME`         | `token_insert`         | Name of the PostgreSQL NOTIFY channel to listen for notifications.         |
| `MB_QUEUE_NAME`           | `user_action_queue`    | Name of the PostgreSQL queue or table for storing user actions.            |
| `MB_HEALTHCHECK_INTERVAL` | `270000` (4.5 minutes) | Interval in milliseconds for health checks on the database connection.     |
| `MB_BATCH_TIMEOUT`        | `5000` (5 seconds)     | Timeout in milliseconds to wait for accumulating a batch of notifications. |
| `MB_BATCH_LIMIT`          | `10`                   | Maximum number of items to process in a single batch.                      |

### watcher

| Name                      | Default Value       | Description                                                                          |
| ------------------------- | ------------------- | ------------------------------------------------------------------------------------ |
| `MB_DEBUG`                | `false`             | Enables debug mode, logging requests and responses to stdout without sending emails. |
| `MB_SES_CONFIG_SET`       | `default`           | Name of the SES configuration set to use for sending emails.                         |
| `MB_SES_SOURCE`           | `noreply@localhost` | Email address used as the sender.                                                    |
| `MB_RESPONSE_OUTPUT_PATH` | `./output`          | Directory path for saving HTTP responses from SES.                                   |
