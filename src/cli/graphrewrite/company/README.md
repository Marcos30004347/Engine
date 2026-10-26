# Small Team, Big Cloud

A seeded, interactive 52-week story about a film streaming startup or fantasy game studio. A small cast
of seed-generated employees and simulated AI agents plans work, builds a product,
launches it, then lives with the consequences. Both paths begin as small software startups with no product or customers. The two playable paths have different long-running workloads and rival companies.

From the repository root:

```sh
cmake -S . -B build
cmake --build build --target company-story CompanyStoryTests -j 4
./build/src/cli/graphrewrite/company-story
```

Choose a seed and path. **Up/Down or j/k** moves the selection; **Enter** confirms.
Numbers select an option before Enter. After answering, the terminal clears and
the next passage appears with a short typewriter animation. **Space or Enter**
finishes the animation immediately. The current phase, week, cash, recurring
weekly costs, employee/agent counts and customer count appear once at the top of each page. Architecture stays at the company desk.

Press **Ctrl+A** or **a** for the architecture board, or an uppercase node label such as **S**
to inspect that component directly. The board opens a system map (accounts, catalogue, payments, playback, workers and operations); selecting a system drills into its shared AWS components. Boxes and routed arrows show the connections. **Command+A** works when the terminal forwards CSI-u Super+A; otherwise map Command+A to Ctrl+A in your terminal settings. Many terminals reserve it for Select All. These shortcuts work at story and Friday
prompts. Press **m at any decision** to open the company desk without answering the pending
question. Return to resume the conversation. **q or Ctrl-C** exits cleanly,
including during animation. The desk is also offered at the end of every week.

```sh
./build/src/cli/graphrewrite/company-story --seed 42 --path video
./build/src/cli/graphrewrite/company-story --seed 42 --path game
./build/src/cli/graphrewrite/company-story --no-animation
./build/src/cli/graphrewrite/company-story --plain
./build/src/cli/graphrewrite/company-story --demo --seed 42 --out /tmp/company-42.txt
./build/src/cli/graphrewrite/company-story --list-paths
```

`--no-animation` retains arrow keys and screen clearing. `--plain`, redirected
input and `TERM=dumb` use numbered line input, with no clearing or delays. `m`
works in plain mode too. `--demo` plays a repeatable prudent strategy without
input, clearing or delays. `--out` overwrites the specified file with an unanimated
chronicle of scenes, choices, status and weekly accounts. It is not a resumable
save: replay requires the same seed and choices.

The same game is available through a rebuilt `graphrewrite-story --company`.
The existing batch chronicle mode is unchanged. This executable now links the graph-rewrite runtime, so the former standalone five-file compile command is no longer supported.

## Rules and narration

`assets/graphrewrite/company.graphrewrite` is compiled by the existing compiler and evaluated by `StoryRunner`. `company.narration` is rendered by the existing `Narrator`, including graph-bound names such as `{C}`. No LLM or network is required.

The host publishes a snapshot of company ownership, component connections, configuration, phase and clock facts for each query. Rules create event records by matching that graph. The first applicable rule in source order selects the result within a query domain; specific cases precede fallbacks. Snapshots are rebuilt after decisions, so repaired configurations cannot leave stale complaint matches.

Rules drive introductions, Mondays, phase chapters, weekly scenes, market news, development overruns, hiring suggestions, architectural incidents, exercises and partner complaints. The host retains numeric accounting, staffing/work simulation, decision menus and applying effects. This is a synchronous rules adapter; it does not keep a separate persistent graph world between prompts.

The `# @effect` annotation is host metadata alongside each rule: rule ID, cost, disruption days, trust loss, cost per 100 customers, breach flag and optional action. Narration belongs in the companion file. Costs are fictional game values. Edit both files and restart; no C++ rebuild is needed for story rules or prose. Set `COMPANY_STORY_RULES=/path/company.graphrewrite` to load another programme and its sibling `.narration`; invalid assets fail with diagnostics instead of falling back silently.

Partner complaints require a matching architectural pattern, such as public delivery connected to licensed storage, live Deep Archive behind a CDN, or an API handing long jobs to Lambda. Choosing a contextual shortcut alone does not trigger a complaint. The explanation identifies the connected components, the violated workload requirement and a repair.

## The weekly story

**Development** starts with $85,000, no paying customers, and a system-design blueprint. Choose API ownership and datasets, refine the graph into AWS services, and assign implementation work. Development infrastructure costs money before subscriptions earn it. Architecture choices influence implementation and operation but never grant automatic product completion. Use `--difficulty hard`, `normal` (default), or `easy` for deadlines of 12, 18, or 24 weeks from the start. Missing the deadline costs $1,000 per additional development week; finishing early is allowed.

**Launch** requires all seven core project tasks. You can rehearse longer before opening subscriptions. Quality and architecture determine how well the first customers' traffic is handled.

**Maintenance** starts the following week. Scenes include growth, partner
relationships, workload, recovery preparation, breaches, quiet successes and
service interruptions. The alien regional-abduction event happens in week 20 if
the company is already in maintenance. An outage can span multiple weeks;
revenue remains reduced until recovery finishes. Surviving 52 weeks produces an
ending reflecting the company; staying in development has a distinct ending.

Every week has a Monday planning board, a company decision, task-specific implementation questions,
Thursday events and Friday accounts. Incidents and salary requests can introduce
extra decisions. Previous events, phase, team composition and player choices feed
into later scenes. Narrative and models are local simulations, with no LLM or AWS
account required.

## Weekly planning and implementation

At the beginning of each week, assign one available task to the founder and each
hired colleague, or leave their capacity free. A task has one owner per week and
prerequisites; completed work remains delivered across weeks. Unfinished tasks
need a new weekly assignment. The core system includes:

- Catalogue API and data model.
- Account registration and authentication.
- Subscription/payment API (needs accounts).
- Playback app or game client (needs catalogue and accounts).
- Video ingestion/encoding or persistent game sessions (needs catalogue).
- Deployment and monitoring (needs accounts).
- Beta onboarding, consent and rights (needs accounts and billing).

Once the app is live, recovery rehearsals, reliability and customer research provide ongoing work. The recovery task leaves the planning queue after a successful rehearsal and returns when the recovery strategy changes.

Assigned workers encounter implementation details in their own tasks. The founder
asks the player; more capable employees and models decide more often themselves.
Technical tasks use technical capability, while onboarding/consent and customer
work use contextual judgement. Autonomous decisions can be right or wrong.
Supervision reduces agent mistakes but does not guarantee correctness. Worker
completion messages do not reveal which autonomous choices were mistakes: those
choices update the real architecture, and later incidents can expose them.

If an owner leaves during a prompt, their work stops and remains available for
reassignment. No assignments means no product delivery. Waiting, hiring someone
without giving them work, or repeatedly choosing a scope option cannot make the
system launchable.

Daily screens show **not started / in development / delivered**, without delivery
percentages or numerical progress rewards in choices. Every four weeks, a
**monthly review** reports overall delivery, per-task percentages, quality, trust,
and the month's revenue/costs. The company desk can reopen the last monthly
snapshot; it does not reveal fresh between-review progress figures. Cash,
customers and costs remain visible throughout play.

## Design APIs first, then choose services

Development opens with a system-design workshop: one shared server/database, or separate payments compute and invoice storage. The nodes start **undecided**. Follow-up decisions choose AWS services for active unresolved nodes; launching requires both completed work and a resolved request path. Implementation questions follow the compute and data ownership of the task being built.

Press **Ctrl+A**, then **8 — Edit API ownership, datasets and service nodes**. The editor provides:

- APIs `U1`–`U5`: Accounts, Catalogue, Payments, Playback/Matchmaking and Recommendations. Each records its workload, compute owner and dataset dependencies.
- Datasets `B1`–`B4`: Members, Catalogue, Subscriptions/Invoices and Viewing History. Each has an access pattern and its own database placement.
- Compute `A`, `C2`, etc.: choose Lambda, EC2 or Fargate; extract one API into a dedicated service, or move it to an existing node.
- Databases `D`, `D2`, etc.: choose DynamoDB, RDS or Aurora PostgreSQL; move individual datasets, migrate the entire engine, configure relational pooling, or retire an unused node.

The renderer derives physical edges from API ownership and dataset placement. For example, move Payments to Lambda `C2` and invoices `B3` to Aurora `D2`: `C2` still reads Members `B1` from DynamoDB `D`, while also connecting to `D2`. Other APIs keep their compute owners. Moving a shared dataset moves its consumers' connections together; this demo does not model replicas or two simultaneous authoritative copies.

Use arrows, numbers or node labels (`U3`, `B3`, `D2`). In raw terminal mode a complete unique label selects immediately; press Enter for a label that is a prefix of another, such as `D` when `D2` exists. The editor caps compute plus database nodes at ten. Extra configured nodes incur base costs even when unused; retiring unused nodes stops billing. Original `A`/`D` anchors become undeployed placeholders when retired.

Live API/compute cutovers cost $900, verified data/engine migrations $1,200, quick data cutovers $250, and reconciliation $600. Blueprint edits are free. A repeated selection is free. Quick cutovers can leave unverified records or unsuitable query models; later `.graphrewrite` events explain the actual affected API, dataset and backend. A $200 topology rehearsal uses those same patterns without causing a production incident. Monthly prices are game values, not AWS quotes.

The rules distinguish workload design from product names: DynamoDB can support relational data when access patterns are adapted ([AWS data-modeling example](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/bp-modeling-nosql-B.html)). The Lambda-to-SQL burst scenario models connection pressure and pooling, consistent with [AWS Lambda/RDS guidance](https://docs.aws.amazon.com/lambda/latest/dg/services-rds.html).

## The architecture is part of the game

```text
Clients -> [E:edge] -> [S:content storage]
Clients -> [A:API]  -> [D:accounts/entitlements]
              |----> [W:long-running workers] -> [S]
[P:capacity] [I:IAM] [O:observability] [R:recovery]
```

| Label | Editable choices |
| --- | --- |
| G | API Gateway routing derived from API ownership |
| A, C2… | Independently selected Lambda, single/multi-AZ EC2/ALB, ECS Fargate |
| E | CloudFront with private origin and signed viewers, public S3, application proxy |
| S | S3 Standard, Intelligent-Tiering without optional archives, Standard-IA, Deep Archive, EBS/EC2 origin |
| D, D2… | DynamoDB, private multi-AZ RDS, Aurora PostgreSQL, public single-AZ RDS; per-backend pooling |
| W | Video: MediaConvert/queue, Lambda, EC2; game: persistent Fargate sessions, Lambda, EC2 |
| P | Bounded demand scaling, fixed capacity, aggressive high limits, always-warm peak capacity |
| I | Scoped roles or broad/shared credentials |
| R | One Region, cross-Region backups, pilot light, warm standby, active/active |
| O | Redacted logs/alerts/budgets, full sensitive request logging, minimal monitoring |

The graph, bill and event evaluator read the same configuration. Each node shows
its settings and monthly cost. Edges describe logical request and control paths;
this is a high-level game model, not an AWS deployment template. [A] is the short
account API; [W] runs long-lived sessions or encoding, so Lambda can suit [A]
while being wrong for the entire long-running job. EBS includes an EC2 content
origin; it is not treated as a directly addressable S3 bucket.

Production configuration changes cost $900 per changed node; IAM setup is $600.
Selecting the current configuration does not charge a migration or invalidate
an existing recovery rehearsal. Incident remediation includes its own repair
cost and does not automatically fix capacity or storage-tier choices.

Use a node's **tabletop event** option to pay $200 for an assessment from the same
rules used by live incidents. Exercises report estimated cost/downtime but do
not cause an outage or breach. They are deliberately not real production chaos
experiments. Live scenarios cover traffic surges, public data paths and leaked
credentials, AZ loss, archive restores, retrieval fees, log/retry bills,
long-running jobs, database exposure and regional recovery. Expensive choices
can waste money without being insecure; data exposure is modeled separately.

Every eight weeks, an industry contact shares a rival design review. **Silverline**
competes with the video company; **Brightforge** competes with the game studio.
The report compares a concrete change at your current customer count, reports
higher costs honestly when a resilience choice costs more, and lets you adopt
that choice. The full rival reference design also has a computed recurring bill.
The rival's audience grows through the campaign. These fictional reports do not
claim that one service is universally optimal for every workload.

## Demo pacing

The campaign has 52 weekly chapters, rotating decision scenes, worker questions,
architecture incidents, six rival reports, and end-of-week management. It targets
roughly **45-75 minutes** for a person reading, thinking and inspecting designs.
This is a design estimate, not a measured playtest guarantee. No timer forces
players to wait; animation is skippable, and `--demo` is an automated showcase
that completes quickly. Reckless choices can end a company much earlier.

## People, models and the company desk

The cast is limited to six colleagues plus the unpaid founder. Every hire adds a weekly assignment slot, including hires made while Monday planning is interrupted. Ready work beyond current staffing prompts a hiring suggestion. Each visit to the hiring desk generates two fresh human applicants with seed-generated names, adult ages,
experience, skills and salary expectations; their identities stay stable within
a campaign. AI model names remain recognizable product names. Humans also have
morale. Overwork, outages and declined
raises can lead to resignations; rest and salary increases help retention.
Each human is eligible for a salary conversation only after 52 weeks since hiring
or their previous annual review. Requests are staggered by tenure rather than
repeated every few weeks. They may request 15% raises. Approved salaries take effect next week.

AI models have names, months since activation, technical/contextual capabilities
and subscription costs. Cheaper agents ask more questions and are less reliable
without review. New models arrive in weeks 10 and 18 with different capabilities
and prices. Provider price notices arrive every eight weeks starting in week 7 and affect the
selected model's next renewal, including active agents.

The company desk lets you:

- Hire from the current market, lay off a human or stop an agent.
- Inspect weekly costs, cash, committed payroll, expected revenue and runway.
- Pitch investors: $25,000 for 10 ownership points, or a qualified $70,000 growth
  round for 25 points. Pitches cost money and may be rejected. There is a four-week
  cooldown and a maximum of three successful rounds. Capital is not revenue.
- Change cloud architecture, production supervision or team workload.

Human layoffs pay one week of severance immediately. Stopping an agent has no
termination fee. A worker engaged during a week remains payable for that week,
so dismissal cannot erase work already received. Future recurring costs change
immediately in the forecast. Departed workers have a rehiring cooldown; humans
who quit need a longer break. Dismissing the speaker while a question is pending
removes that worker's question when you return to the story.

## Accounting and checks

A game month is four weeks. $12 monthly subscriptions accrue at $3 per week,
less outage credits. Monthly staff and cloud rates are converted to weekly
charges. Launch introduces film rights at $900/month plus $2/customer/month.
One-off decisions deduct cash immediately and appear in the next Friday ledger;
they are not deducted twice. The forecast excludes unknown incidents and
investment. Cash at or below zero after settlement ends the campaign.

Prices, capability scores, investment terms, risk probabilities and recovery
times are fictional game values, not provider quotes or promises.

```sh
ctest --test-dir build -R '^CompanyStory' --output-on-failure
```

The simulation tests cover deterministic replay, random employees, the startup-to-launch
transition, task dependencies, assignments, latent defects, monthly reporting, launch
consequences, multi-week recovery, accounting, dismissal commitments, resignations,
raises, model releases/renewals, investment cooldowns/dilution and bankruptcy.
CLI tests cover a complete campaign, transcripts, invalid arguments, desk
interruptions, arrow keys, screen clearing, animation skip, and terminal restoration
on normal exit and interruption. POSIX terminal checks skip Windows.

`Company.cpp` contains the company simulation and scenes; `Planning.cpp` owns
tasks, dependencies, assignment and worker implementation. `Architecture.cpp`
contains the graph, costs, rival comparisons and shared incident/tabletop rules.
`CompanyCli.cpp` handles
presentation and input. This is independent of the batch graph-rewrite runtime.

AWS references behind the fictional scenarios:

- [Disaster recovery strategies](https://docs.aws.amazon.com/whitepapers/latest/disaster-recovery-workloads-on-aws/disaster-recovery-options-in-the-cloud.html).
- [Private S3 origins and OAC](https://docs.aws.amazon.com/AmazonCloudFront/latest/DeveloperGuide/private-content-restricting-access-to-s3.html).
- [Lambda execution limits](https://docs.aws.amazon.com/lambda/latest/dg/gettingstarted-limits.html).

Additional AWS references used for the architecture scenarios:

- [S3 storage classes](https://docs.aws.amazon.com/AmazonS3/latest/userguide/storage-class-intro.html).
- [Archive retrieval](https://docs.aws.amazon.com/AmazonS3/latest/userguide/restoring-objects-retrieval-options.html).
- [EBS volumes and Availability Zones](https://docs.aws.amazon.com/ebs/latest/userguide/ebs-volumes.html).
- [EC2 target tracking](https://docs.aws.amazon.com/autoscaling/ec2/userguide/as-scaling-target-tracking.html).
- [ECS capacity and availability](https://docs.aws.amazon.com/AmazonECS/latest/developerguide/capacity-availability-best-practice.html).

## Architecture reviews and reports

Open the architecture map with Ctrl+A or `a`, then choose **9** for the report. Monthly reviews include a snapshot too. Reports show each API's compute and dataset dependencies, monthly costs by node (shared resources count once), idle resources, readiness, recovery configuration, and the last twelve topology changes with recurring and one-off costs.

Architecture review scenes use `.graphrewrite` patterns over the current graph. They identify a connected public database, an unmodeled or unverified dataset, Lambda-to-SQL connection pressure, idle billed capacity, or single-zone compute. Narration names the matched API, dataset or service, and the decision opens that exact node. Repairing the graph changes subsequent reviews and incident eligibility. A quiet review is an observation, not a guarantee that the architecture is safe.

## Reading and accepting decisions

In an interactive terminal, questions and choices animate, with a green pointer on the selected row. Use Up/Down (or j/k) and Enter; the full selected description appears below the list. Cyan headers and yellow questions distinguish company status from choices. Space or Enter finishes the current page's animation. A result stays visible until you continue before the next page clears it. `--no-animation` keeps colors and arrow navigation; `--plain` and redirected input have no terminal escape codes. Empty input accepts the first option in plain mode too.

Architecture and implementation questions can open with a named human or agent's proposal. **Continue with this decision** accepts it; **Choose another option** opens the original settings. Proposals are deterministic for the seed, week, author and decision; reopening one does not reroll it or spend money. Technical/contextual abilities affect the proposal, without guaranteeing a good outcome. Autonomous task decisions are recorded in the narrative and affect the same architecture graph. Repeated Enter presses advance through planning, proposals and story pages, but can still lead to bankruptcy.
