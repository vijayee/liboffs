Here's the Meridian paper as markdown:

```markdown
# Meridian: A Lightweight Framework for Network Positioning without Virtual Coordinates

**Bernard Wong, Aleksandrs Slivkins, Emin Gün Sirer**

Dept. of Computer Science, Cornell University, Ithaca, NY 14853

{bwong, slivkins, egs}@cs.cornell.edu

February, 2005

---

## Abstract

Selecting nodes based on their position in the network is a basic building block for many distributed systems. This paper describes a peer-to-peer overlay network for performing position-based node selection. Our system, Meridian, provides a lightweight, accurate and scalable framework for keeping track of location information for participating nodes. The framework consists of an overlay network structured around multi-resolution rings, query routing with direct measurements, and gossip protocols for dissemination. We show how this framework can be used to address three commonly encountered problems in large-scale distributed systems without having to compute absolute coordinates; namely, closest node discovery, central leader election, and locating nodes that satisfy target latency constraints. We show analytically that the framework is scalable with logarithmic convergence when Internet latencies are modeled as a growth-constrained metric, a low-dimensional Euclidian metric, or a metric of low doubling dimension. Large scale simulations, based on latency measurements from 6.25 million node-pairs, and an implementation deployed on PlanetLab both show that the framework is accurate and effective.

---

## 1. Introduction

A central problem in distributed systems is to find an efficient mapping of system functionality onto nodes based on network characteristics. In small systems, it is possible to perform extensive measurements and make decisions based on global information. For instance, in an online game with few servers, a client can simply measure its latency to all servers and bind to the closest one for minimal response time. However, collecting global information is infeasible for a significant set of recently emerging large-scale distributed applications, where global information is unwieldy and lack of centralized servers makes it difficult to find nodes that fit selection criteria. Yet many distributed applications, such as filesharing networks, content distribution networks, backup systems, anonymous communication networks, pub-sub systems, service discovery, and multi-player online games, could substantially benefit from selecting nodes based on their location in the network.

A general technique for finding nodes that optimize a given network metric is to perform a network embedding, that is, to map high-dimensional network measurements into a location in a smaller Euclidian space. For instance, recent work in network positioning maps a large vector of node-to-node latency measurements on the Internet into a single point in a d-dimensional space. The resulting embedded address facilitates location-aware node selection.

While this approach is quite general, it is neither accurate nor complete. The embedding process typically introduces significant errors. The selection of parameters, such as the constant d and the set of measurements taken to perform the embedding, is nontrivial and has a significant impact on the accuracy of the approach. Coordinates change over time due to changes in network latencies on the Internet, and introduce additional errors when performing latency estimates from coordinates computed at different times. Finally, finding a set of nodes that match desired criteria without centralized servers that retain O(N) state, an essential requirement in large-scale networks, requires additional mechanisms besides virtual coordinates. Peer-to-peer substrates that can naturally work with Euclidian coordinates, such as CAN and P-Trees, can reduce the state requirements per node; however, both systems introduce substantial complexity and bandwidth overhead in addition to the overhead of network embedding. And our simulation results show that even with a P2P substrate that always finds the best node based on virtual coordinates, the embedding error leads to a suboptimal choice.

This paper introduces a lightweight, scalable and accurate framework, called Meridian, for performing node selection based on a set of network positioning constraints. Meridian is based on a loosely-structured overlay network, uses direct measurements instead of a network embedding, and can solve spatial queries without an absolute coordinate space. It is similar in functionality to GNP combined with CAN in performing node selection based on network location.

Meridian is lightweight, scalable and accurate. Each Meridian node keeps track of a fixed number of peers and organizes them into concentric rings of exponentially increasing radii. A diverse node selection protocol is used in determining ring membership to maximize the marginal utility provided by each ring member. A query is matched against the relevant nodes in these rings, and optionally forwarded to a subset of the node's peers. Intuitively, the forwarding "zooms in" towards the solution space, handing off the query to a node that has more information to solve the problem due to the structure of the peer set. A scalable gossip protocol is used to notify other nodes of membership in the system. Meridian avoids incurring embedding errors by making no attempt to reconcile the latencies seen at participating nodes into a globally consistent coordinate space. Directly evaluating queries against relevant peers in each ring further reduces errors stemming from out of date coordinates.

In this paper, we focus on three commonly-encountered network positioning problems in distributed systems, and describe how the lightweight Meridian framework can be used to resolve them without computing virtual coordinates. The first, and most significant, problem is that of discovering the closest node to a targeted reference point. This is a basic operation in content distribution networks (CDNs), large-scale multiplayer games, and peer-to-peer overlays. Having the closest node serve the client or operate on the target can significantly reduce response time and aggregate network load. For instance, a geographically distributed peer-to-peer web crawler can reduce crawl time and minimize network load by delegating the crawl to the closest node to each target web server. Similarly, CDNs take network position into account when assigning clients to servers. And multiplayer games often perform a similar mapping from clients to nearby servers. In fact, the closest node discovery problem is so pervasive and so significant that we examine it in great detail. We also show that the Meridian framework can be used to find a node that offers minimal latencies to a given set of nodes. Intuitively, we want to select a node that is at the centerpoint of the region defined by the set members. This basic operation can be used, for instance, for location-aware leader election, where it would enable the chosen leader to minimize the average communication latency from the leader to set members. Such an operation can be used in tree construction for an application-level multicast system, where it can reduce transmission latencies by placing centrally-located nodes higher in the tree. Finally, we examine the problem of finding a set of nodes in a region whose boundaries are defined by latency constraints. For instance, given a set of latency constraints to well-known peering points, we show how Meridian can locate nodes in the region defined by the intersection of these constraints. This functionality is useful for ISPs and hosting services to cost effectively meet service-level agreements, for computational grids that can sell node clusters with specific inter-node latency requirements, and generally, for applications that require fine-grain selection of services based on latency to multiple targets.

We demonstrate through a theoretical analysis that our system provides robust performance, delivers high scalability and balances load evenly across the nodes. The analysis ensures that the performance of our system is not an artifact of our measurements.

We evaluate Meridian through simulation parameterized by a large-scale network measurement study, and through a deployment on PlanetLab. For our measurement study, we collected node-to-node round-trip latency measurements for 2500 nodes and 6.25 million node pairs on the Internet using the King measurement technique. We use 500 of these nodes as targets, and the remaining 2000 as the overlay nodes in our experiments.

Overall, this paper makes three contributions. First, it outlines a lightweight, scalable, and accurate system for keeping track of location-information for participating nodes. The system is simple, loosely-structured, and entails modest resources for maintenance. The paper shows how Meridian can efficiently find the closest node to a target, the latency minimizing node to a given set of nodes, and the set of nodes that lie in a region defined by latency constraints, frequently encountered building block operations in many location-sensitive distributed systems. Although less general than virtual coordinates, we show that Meridian incurs significantly less error. Second, the paper provides a theoretical analysis of our system that shows that Meridian provides robust performance, high scalability and good load balance. This analysis is general and applies to Internet latencies that cannot be accurately modeled with a Euclidean metric. Following a line of previous work on object location, we give guarantees for the family of growth-constrained metrics. Moreover, we support a much wider family of metrics of low doubling dimension which has recently become popular in the theoretical literature. Finally, the paper shows empirical results from both simulations using measurements from a large-scale network study and a PlanetLab deployment. The results confirm our theoretical analysis that Meridian is accurate, scalable, and load-balanced.

---

## 2. Framework

The basic Meridian framework is based around three mechanisms: a loose routing system based on multi-resolution rings on each node, an adaptive ring membership replacement scheme that maximizes the usefulness of the nodes populating each ring, and a gossip protocol for node discovery and dissemination.

### Multi-Resolution Rings

Each Meridian node keeps track of a small, fixed number of other nodes in the system, and organizes this list of peers into concentric, non-overlapping rings. The i-th ring has inner radius β·α^(i-1) and outer radius β·α^i, for i ≥ 1, where β is a constant, α is the multiplicative increase factor, and β·α^0 = β for the innermost ring. Each node keeps track of a finite number of rings; all rings i > i_max for a system-wide constant i_max are collapsed into a single, outermost ring that spans the range [β·α^(i_max), ∞).

Meridian nodes measure the distance d_j to a peer j, and place that peer in the corresponding ring i such that β·α^(i-1) ≤ d_j < β·α^i. This sorting of neighbors into concentric rings is performed independently at each node and requires no fixed landmarks or distributed coordination. There is an upper limit of k on nodes kept in each ring, where peers are dropped from overpopulated rings; consequently, Meridian's space requirement per node is proportional to k. We later show in the analysis (Section 4) that a choice of k = O(log N) can resolve queries in O(log N) lookups; in simulations (Section 6), we verify that a small k suffices. We assume that every participating node has a rough estimate of the maximum size of the system.

The rationale for exponentially increasing ring radii stems from the need for a node to have a representative set of pointers to the rest of the network, and the higher marginal utility nearby peers offer over faraway ones. The ring structure favors nearby neighbors, enabling each node to retain a relatively large number of pointers to nodes in their immediate vicinity. This allows a node to authoritatively answer geographic queries for its region of the network. At the same time, the ring structure ensures that each node retains a sufficient number of pointers to remote regions, and can therefore dispatch queries towards nodes that specialize in those regions. An exponentially increasing radius also makes the total number of rings per node manageably small and i_max clamps it at a constant.

### Ring Membership Management

The number of nodes per ring, k, represents an inherent tradeoff between accuracy and overhead. A large k increases a node's information about its peers and helps it make better choices when routing queries. On the other hand, a large k also entails more state, more memory and more bandwidth at each node.

Within a given ring, node choice has a significant effect on the performance of the system. For instance, if the nodes within a given ring are clustered together, their marginal utility is very small, despite their additional cost. A key principle then is to promote geographic diversity within each ring.

Meridian achieves geographic diversity by periodically reassessing ring membership decisions and replacing ring members with alternatives that provide greater diversity. Within each ring, a Meridian node not only keeps track of the k primary ring members, but also a constant number l of secondary ring members, which serve as a FIFO pool of candidates for primary ring membership.

We quantify geographic diversity through the hypervolume of the k-polytope formed by the selected nodes. To compute the hypervolume, each node defines a local, non-exported coordinate space. A node A will periodically measure its distance d_{A,j} to another node j in its ring, for all 1 ≤ j ≤ k+l. The coordinates of node A consist of the tuple (d_{A,1}, d_{A,2}, ..., d_{A,k+l}), where d_{A,A} = 0. This embedding is trivial to construct and does not require a potentially error-introducing mapping from high-dimensional data to a lower number of dimensions.

Having computed the coordinates for all of its members in a ring, a Meridian node can then determine the subset of k nodes that provide the polytope with the largest hypervolume. For small k, it is possible to determine the maximal hypervolume polytope by considering all possible polytopes from the set of k+l nodes. For large k+l, evaluating all subsets is infeasible. Instead, we take a simple, greedy approach: A node starts out with the k+l polytope, and iteratively drops the vertex (and corresponding dimension) whose absence leads to the smallest reduction in hypervolume until k vertices remain. The remaining vertices are designated the new primary members for that ring, while the remaining l nodes become secondaries. This computation can be performed in linear time using standard computational geometry tools. The ring membership management occurs in the background and its latency is not critical to the correct operation of Meridian. Note that the coordinates computed for ring member selection are used only to select a diverse set of ring members — they are not exported by Meridian nodes and play no role in query routing.

Churn in the system can be handled gracefully by the ring membership management system due to the loose structure of the Meridian overlay. If a node is discovered to be unreachable during the replacement process, it is dropped from the ring and removed as a secondary candidate. If a peer node is discovered to be unreachable during gossip or the actual query routing, it is removed from the ring, and replaced with a random secondary candidate node. The quality of the ring set may suffer temporarily, but will be corrected by the next ring replacement. Discovering a peer node failure during a routing query can reduce query performance; k can be increased to compensate for this expected rate of failure.

### Gossip Based Node Discovery

The use of a gossip protocol to perform node discovery allows the Meridian overlay to be loosely connected, highly robust and inexpensively kept up-to-date of membership changes. Our gossip protocol is based on an anti-entropy push protocol that implements a membership service. The central goal of our gossip protocol is not for each node to discover every node in the system, but simply for each node to discover a sufficiently diverse set of other nodes.

Our gossip protocol works as follows:

1. Each node A randomly picks a node B from each of its rings and sends a gossip packet to B containing a randomly chosen node from each of its rings.
2. On receiving the packet, node B determines through direct probes its latency to A and to each of the nodes contained in the gossip packet from A.
3. After sending a gossip packet to a node in each of its rings, node A waits until the start of its next gossip period and then begins again from step 1.

In step 2, node B sends probes to A and to the nodes in the gossip packet from A regardless of whether B has already discovered these nodes. This re-pinging ensures that stale latency information can be replaced as latency between nodes on the Internet changes dynamically. The newly discovered nodes are placed on B's rings as secondary members, subject to FIFO replacement.

For a node to initially join the system, it needs to know the IP address of one of the nodes in the Meridian overlay. The newly joining node contacts the Meridian node and acquires its entire list of ring members. It then measures its latency to these nodes and places them on its own rings; these nodes will likely be binned into different rings on the newly joining node. From there, the new node participates in the gossip protocol as usual.

The period between gossip cycles is initially set to a small value in order for new nodes to quickly propagate their arrival to the existing nodes. The new nodes gradually increase their gossip period to the same length as the existing nodes. The choice of a gossip period depends on the expected rate of latency change between nodes and expected churn in the system.

---

## 3. Applications

The following three sections describe how Meridian can be used to solve some common network positioning problems.

### Closest Node Discovery

Meridian locates the closest node by performing a multi-hop search where each hop exponentially reduces the distance to the target. This is similar to searching in structured peer-to-peer networks such as Chord, Pastry and Tapestry, where each hop brings the query exponentially closer to the destination, though in Meridian the routing is performed using physical latencies instead of numerical distances in a virtual identifier space. Another important distinction that Meridian holds over the structured peer-to-peer networks is the target nodes need not be part of the Meridian overlay. The only requirement is that the latency between a node on the overlay and a target node can be measured. This enables applications such as finding the closest node to a public web server, where the web server is not directly controlled by the distributed application and only responds to HTTP queries.

When a Meridian node receives a client request to find the closest node to a target, it determines the latency d between itself and the target. Once the latency is determined, it locates its corresponding ring i and simultaneously queries all nodes in that ring, as well as all nodes in the adjacent rings i-1 and i+1 whose distances to the origin are within d/2 to 2d. These nodes measure their distance to the target and report the result back to the source. Nodes that take more than 2d to provide an answer are ignored, as they cannot be closer to the target than the source.

Meridian uses an acceptance threshold β, which serves a purpose similar to the routing base in structured peer-to-peer systems; namely, it determines the reduction in distance at each hop. The route acceptance threshold is met if one or more of the queried peers is closer than β times the distance to the target, and the client request is forwarded to the closest node. If no peers meet the acceptance threshold, then routing stops and the closest node currently known is chosen.

Meridian is agnostic to the choice of a route acceptance threshold β, where 0 ≤ β < 1. A smaller β value reduces the total number of hops, as fewer peers can satisfy the requirement, but introduces additional error as the route may be prematurely stopped before converging to the closest node. A larger β may reduce error at the expense of increased hop count.

### Central Leader Election

Another frequently encountered problem in distributed systems is to locate a node that is "centrally situated" with respect to a set of other nodes. Typically, such a node plays a specialized role in the network that requires frequent communication with the other members of the set; selecting a centrally located node minimizes both latency and network load. An example application is leader election, which itself is a building block for higher level applications such as clustering and low latency multicast trees.

The central leader election application can be implemented by extending the closest node discovery protocol. We replace d in the single target closest node selection protocol with d_avg in the multi-target protocol. When a Meridian node receives a client request to find the closest node to the target set S, it determines the latency set {d_1, ..., d_{|S|}} between itself and the targets through direct measurements, and computes the average latency d_avg = (1/|S|) Σ_{i=1}^{|S|} d_i. Similarly, when a ring member is requested to determine its latency to the targets, it computes the average latency and returns that to the requesting node. The remaining part of the central leader election application follows exactly from the closest node discovery protocol.

### Multi-Constraint System

Another frequent operation in distributed systems is to find a set of nodes satisfying constraints on the network geography. For instance, an ISP or a web hosting service is typically bound by a service level agreement (SLA) to satisfy latency requirements to well-known peering locations when hosting services for clients. A geographically distributed ISP may have thousands of nodes at its disposal, and finding the right set of nodes that satisfy the given constraints is necessary for satisfying the SLA. Latency constraints are also important for grid based distributed computation applications, where the latency between nodes working together on a problem is often the main efficiency bottleneck. A customer may want to specify that ∀a,b ∈ G, where G is the set of grid nodes, latency(a,b) ≤ L for some desired latency L.

Finding a node that satisfies multiple constraints can be viewed as a node selection problem, where the constraints define the boundaries of a region in space (the solution space). A constraint is specified as a target and a latency bound around that target. When a Meridian node receives a multi-constraint query with m constraints specified as {(T_k, range_k)}, for all k = 1...m, it measures its latency d_k to the target nodes and calculates its distance to the solution space as:

```
dist = Σ_{k=1}^{m} max(0, d_k - range_k)
```

If dist is 0, then the current node satisfies all the constraints, and it returns itself as the solution to the client. Otherwise, it iterates through all its peers, and simultaneously queries all peers j that are within [max(0, d_k - range_k), 2·d_k + range_k] from itself, for all k = 1...m. These nodes include all the peers that lie within the range of at least one of the constraints, and possibly other peers that do not satisfy any of the constraints, but are nevertheless close to the solution space. These peer nodes measure their distance to the m targets and report the results back to the source. Nodes that take longer than 2·(d_k + range_k) for k = 1...m to provide an answer are ignored.

The distance dist_j of each node j to the solution space is calculated using the same formula. If dist_j is 0, then node j satisfies all the constraints and is returned as a solution to the client. If no zero valued dist_j is returned, the client determines whether there is a dist_j < β·dist, where β is the route acceptance threshold. If the route acceptance threshold is met, the client request is forwarded to the peer closest to the solution space. A larger β may increase the success rate, at the expense of increased hop count.

---

## 4. Analysis of Scalability

In this section we argue analytically that Meridian scales well with the size of the system. Our contributions are three-fold. First, we put forward a rigorous definition that captures the quality of the ring sets, and prove that under certain reasonable assumptions small ring cardinalities suffice to ensure good quality. Second, we show that with these good-quality rings, the nearest-neighbor queries work well, i.e. return exact or near-exact neighbors in logarithmic number of steps. Finally, we argue that if the ring sets of different nodes are stochastically independent then the system is load-balanced, that is if many random queries are inserted into the system then the load is spread approximately evenly among the Meridian nodes.

We model the matrix of Internet latencies as a metric. We should not hope to achieve theoretical guarantees for arbitrary metrics; we need some reasonable assumptions to capture the properties of real-life latencies. We avoid assumptions on the geometry of the metric (e.g. we do not assume it is Euclidean) for two reasons. Firstly, recent experimental results suggest that approximating Internet latencies by Euclidean metrics, although a useful heuristic in some cases, incurs significant relative errors. Secondly, and perhaps more importantly, even if we assume that the metric is Euclidean our algorithm is not allowed to use the coordinates — since one of the goals of this work is precisely to avoid heavy-weight embedding-based approaches.

We will consider two families of metrics that have been popular in the recent systems and theoretical literature as non-geometric notions of low-dimensionality: growth-constrained metrics and doubling metrics. In particular, growth-constrained metrics have been used as a reasonable abstraction of Internet latencies in the analysis of the object-location algorithm of Plaxton et al. Using a more general family of doubling metrics leads to good guarantees even for metrics that combine very dense and very sparse regions.

We focus on the case when the rate of churn and fluctuations in Internet latencies is sufficiently low so that Meridian has ample time to adjust. So for the purposes of this analysis we assume that the node set and the latency matrix are not changing with time.

Full proofs of the following theorems are quite detailed; they are deferred to Appendix A.

### Preliminaries

Nodes running Meridian are called Meridian nodes. When such node receives a query to find the nearest neighbor of some node T, this T is called the target. Let U be the set of all possible targets. Let M ⊆ U be the set of Meridian nodes, of size N. Let d be the distance function on U: denote the A-B-distance by d_{A,B}. Let B_A(r) denote the closed ball in M of radius r around node A, i.e. the set of all Meridian nodes within distance r from A; let B_{A,i} = B_A(β·α^i). For simplicity let the smallest distance be 1; denote the maximal distance by R.

For some fixed k, every node A maintains log_α(R) rings R_{A,i} ⊆ B_{A,i} of exactly k nodes each; the elements of the rings are called neighbors. We treat each ring at a given time as a random variable; in particular, we can talk about a distribution of a given ring, and about rings being probabilistically independent.

### Quality of the Rings

Intuitively, we want each ring R_{A,i} to cover the corresponding ball B_{A,i} reasonably well, e.g. we might want each node in B_{A,i} to be within a small distance from some node in R_{A,i}. Moreover, for load-balancing it is bad if many different queries pass through the same node, so, intuitively, it is desirable that the rings of different nodes are probabilistically independent from each other.

Say a pair A,B of Meridian nodes is ε-nice if node A has a neighbor C within distance ε·d_{A,B} from B, and, moreover, C ∈ R_{A,i} where β·α^(i-1) ≤ d_{A,B}·(1 + ε) < β·α^i; say the rings are ε-nice if all pairs of Meridian nodes are ε-nice.

In Thm. 4.3 and Thm. 4.4a it suffices for the rings to be 1/2-nice; for better precision in a more relaxed model of Internet latencies (see Thm. 4.1) we might need smaller values of ε.

We will show that even with small ring cardinalities it is possible to make the rings ε-nice; this is later confirmed by the empirical evidence in Section 5 (see Fig. 12). We give a constructive argument where we show that the rings with small cardinalities are ε-nice provided that the ring sets (seen as stochastic distributions) have certain reasonable properties.

### 4.1 Growth-Constrained Metrics

Define the Karger-Ruhl dimension (KR-dimension) D as the log of the smallest c such that the cardinality of any ball B_A(r) is at most c times smaller than that of B_A(2r). Say the metric is growth-constrained if D is constant.

Since for a d-dimensional grid the KR-dimension is O(d), growth-constrained metrics can be seen as generalized grids; they have been used as a reasonable abstraction of Internet latencies in past work. Growth-constrained metrics have also been considered in the context of dimensionality in graphs and spatial gossip.

We start with a model where the metric on the Meridian nodes is growth-constrained, but we make no such assumption about the non-Meridian nodes. This is important because even in an unfriendly metric we might be able to choose a relatively well-behaved subset of (Meridian) nodes.

Our first result is that even with small ring cardinalities it is possible to make the rings ε-nice. We say at some point of time the ring R_{A,i} is well-formed if it is distributed as a random k-node subset of B_{A,i}. Intuitively, this is desirable since in a growth-constrained metric the density is more or less uniform.

**Theorem 4.1:** Assume the rings are well-formed; let the metric on Meridian nodes have KR-dimension D. Fix δ > 0 and ε < 1; set k = O((1/ε)^D)·log(N/δ). Then with probability at least 1-δ the rings are ε-nice.

Recall that our nearest-neighbor search algorithm forwards the query to the node C ∈ S that is closest to the target T subject to the constraint that d_{A,C}/d_{B,C} < β_0; if such C does not exist, the algorithm stops. Here β_0 < 1 is a parameter; we denote this algorithm by A(β_0).

Consider a node T and let A be its nearest neighbor. Say node B is a γ-approximate nearest neighbor of T if d_{B,T}/d_{A,T} ≤ γ. Say A is γ-approximate if for any query it finds a γ-approximate nearest neighbor, and does so in at most 2·log_α(R) steps.

**Theorem 4.2:** If the rings are ε-nice, ε < 1/3 then

(a) A(2) is 3-approximate,

(b) A(β_0) is (1+ε)-approximate, β_0 = 1+log ε/2.

(c) if we use a larger threshold β_0 = 1+γ, γ ∈ (ε, 1/2) then A(β_0) is (1+ε+2γ)-approximate.

Note the tradeoff between the threshold β_0 and accuracy of the queries, which matches our simulation in Section 6 (see Figure 8).

In Thm. 4.1 the value of k depends on ε. We can avoid this (and find exact nearest-neighbors) by restricting the model. Specifically, we'll assume that the metric on M ∪ {T} is growth-constrained, for any target T in some set X ⊆ U. However, we do not need to assume that the metric on all of X is growth-constrained; in particular, very dense clusters of targets are allowed.

We'll need to modify A(β_0) slightly: if C is the neighbor of the current node A that is closest to the target T, and d_{A,C}/d_{B,C} ∈ (1, β_0) then instead of stopping at A the algorithm stops at C. Denote this modified algorithm by A'(β_0); say it is X-exact if it finds an exact nearest neighbor for all queries to targets in the set X, and does so in at most log_α(R) steps.

**Theorem 4.3:** Fix some set X ⊆ U such that for any T ∈ X the metric on M ∪ {T} has KR-dimension D. Fix δ > 0, let k = 2·(10)^D·log(N|X|/δ), and assume the rings are well-formed. Then with probability at least 1-δ algorithm A'(2) is X-exact.

Ideally, the algorithm for nearest neighbor selection would balance the load among participating nodes. Intuitively, if C_max(A) is the maximal number of packets exchanged by a given algorithm A on a single query, then for Q random queries we do not want any node to send or receive much more than (Q/N)·C_max(A) packets.

We make it precise as follows. Fix some set X ⊆ U and suppose each Meridian node A receives a query for a random target T_A ∈ X. Say algorithm A is (γ, X)-balanced if in this scenario under this algorithm any given node sends and receives at most γ·C_max(A) packets.

We'll need a somewhat more restrictive model. In particular, we'll assume that the metric on all of X is growth-constrained, and that the rings are stochastically independent from each other. The latter property matches well with our simulation results (see Figure 11).

**Theorem 4.4:** Fix some set X ⊆ U such that the metric on X has KR-dimension D. Suppose M is a random N-node subset of X. Let k = 2·(10)^D·log(|X|/δ)·log(N)·log(R).

(a) If the rings are well-formed then with probability at least 1-δ algorithm A'(2) is X-exact.

(b) If moreover the rings are stochastically independent then with probability at least 1-δ algorithm A'(2) is (γ, X)-balanced, γ = 2·(10)^D·log(N·R/δ).

Note that in Thm. 4.4 it does not suffice to assume that M is an arbitrary subset of X, since in general a subset of a growth-constrained metric can have a very high KR-dimension.

### 4.2 Extensions

Our results allow several extensions. The proofs are omitted from this version of the paper.

(1) Our results hold under a less restrictive definition of KR-dimension that only applies to balls of cardinality at least L = log Q; moreover, we can take L = log(Q·|X|) in Thm. 4.3, and L = log|X| in Thm. 4.4.

(2) We show that if a metric is comparatively 'well-behaved' in the vicinity of a given node, then some of its rings can be made smaller. We'd like the size of R_{A,i} to depend only on what happens in the corresponding ball B_{A,i}. Specifically, for r = ε·β·α^(i-1) we let ρ_{A,i} be the ratio of |B_{A,i}| to the smallest |B_B(r)| such that d_{A,B} < β·α^i - r; note that B_B(r) ⊆ B_{A,i} for any such B. Then in Thm. 4.1 it suffices to assume that the cardinality of each ring R_{A,i} is at least (1/ε)·ρ_{A,i}·log(Q^2/δ).

(3) Our guarantees are worst-case; on average it suffices to query only a fraction of neighbors of a given ring. Recall that on every step in algorithm A(β_0) we look at a subset S of neighbors and forward the query to the node C ∈ S that is closest to the target T subject to the constraint that the progress of C, defined as the ratio d_{A,C}/d_{B,C}, is at least β_0. For β_0 < 2, suppose instead we forward the query to an arbitrary progress-2 node in S if such node exists. It is easy to check that all our results for A(β_0) carry over to this modified algorithm. Moreover, in Thm. 4.2a (used in conjunction with Thm. 4.1 for ε = 1/3) instead of asking all neighbors of a given ring at once, we can ask them in random batches of size k' = O(1); then in expectation one such batch will suffice. Therefore on average on every step (except maybe the last one) we'll use only k' randomly selected neighbors from a given ring. Similarly, we can take k' = O((1/ε)^D) for Thm. 4.2bc (used in conjunction with Thm. 4.1), k' = O(ρ_{A,i}) for Extension (2) above, and k' = O(1) for Thm. 4.3 and Thm. 4.4a. We obtain similar improvements for Thm. 4.2 used with Thm. 4.5 for doubling metrics.

(4) Thm. 4.4b holds under a stronger definition of a (β, X)-balanced algorithm which allows more general initial conditions. Specifically, fix some γ ≥ 0 and Q ≥ N·log(γ, 1), and choose a random partition of Q into N summands q_A, A ∈ M, such that γ ≤ q_A ≤ β·Q/N for each A. Suppose each Meridian node A receives queries for q_A random targets in X. Say algorithm A is (β, γ, Q, X)-balanced if under this algorithm in this scenario any given node sends and receives at most β·C_max(A) packets. Note that an algorithm is (β, X)-balanced if and only if it is (β, 1, N, X)-balanced.

### 4.3 Doubling Metrics

Define the doubling dimension DIM as the log of the smallest c such that every ball B_A(r) can be covered by c balls of radius r/2. Metrics of low doubling dimension is a strictly more general family than growth-constrained metrics since it is easy to see that DIM is at most four times the KR-dimension, but the converse is not true, e.g. for a subset {2^i : 0 ≤ i ≤ N} of the real line DIM = 1, but KR-dimension is 2^N. Intuitively, doubling metrics are more powerful because they can combine very sparse and very dense regions. Moreover, doubling metrics can be seen as a generalization of low-dimensional Euclidean metrics; it is known that for any finite point set in a d-dimensional Euclidean metric DIM = O(d).

Doubling dimension has been introduced in the mathematical literature and has recently become a hot topic in the theoretical CS community; in particular it was used to model Internet latencies in the context of distributed algorithms for embedding and distance estimation.

For metrics of low doubling dimension, well-formed rings are no longer adequate since we need to boost the probability of selecting a node from a sparser region. In fact, this is precisely the goal of our ring-membership management in Section 2. Mathematical literature provides a natural way to make this intuition precise.

Say a measure is c-doubling if for any ball B_A(r) its measure is at most c times larger than that of B_A(r/2). It is known that for any metric there exists a 2^DIM-doubling measure μ. Intuitively, a doubling measure is an assignment of weights to nodes that makes a metric look growth-constrained; in particular, for exponential line μ(2^i) = 2^(-i). Say that at some point of time the ring R_{A,i} is μ-well-formed if it is distributed as a random k-node subset of B_{A,i}, where nodes are drawn with probability μ(x)/μ(B_{A,i}). Using these notions, one can obtain the guarantee in Thm. 4.1 where instead of the KR-dimension we plug in a potentially much smaller DIM of M.

**Theorem 4.5:** Suppose the metric on M has doubling dimension DIM, and let μ be a 2^DIM-doubling measure on M. Fix δ > 0 and ε < 1; set k = O((1/ε)^DIM)·log(N/δ). If the rings are μ-well-formed, then Meridian rings are ε-nice, so Thm. 4.2 applies.

---

## 5. Evaluation

We evaluated Meridian through both a large scale simulation parameterized with real Internet latencies, as well as a physical deployment on PlanetLab.

### Simulation

A large scale measurement study of 2500 DNS servers was used to parameterize simulations for evaluating Meridian. For the study, we collected pairwise round trip time measurements between 2500 servers, spanning approximately 6.25 million node pairs. The study was replicated 9 times from 9 different PlanetLab nodes across North America, with the median value of the 10 runs taken for the round-trip time of each pair of nodes. We verified that each server has a unique IP address to reduce the likelihood that more than one of the chosen servers are hosted on the same machine. The experiment took approximately 8 days to complete, as the query interarrival times were dilated, and queries themselves randomized, to avoid queuing delays at the DNS servers. The experiments were performed from May 5 to May 13, 2004.

We obtained the latency measurement between DNS servers on the Internet via the King measurement technique. King works as follows: assuming that a node A wants to determine the latency between DNS server B and C, it first sends a name lookup request to B and measures distance B-A. Next, a recursive name request is sent to B for a domain where C is the authoritative name server, which will cause B to contact C on the measuring machine's behalf. This request will yield the roundtrip time from the measuring node to C via B, that is B-A + B-C. By taking the difference between the two measured times, A can determine an approximate roundtrip time between B and C.

In the following experiments, each of the tests consist of 4 runs with 2000 Meridian nodes, 500 target nodes, k = 16 nodes per ring, 9 rings per node, size of the innermost ring α = 2, probe packet size of 50 bytes, β = 0.5, and β_0 = 1 ms, for 25000 queries in each run. The results are presented either as the mean result of the 10,000 = 2×5000 queries, or as the mean of the median value of the 4 runs. All references to latency in this section are in terms of round trip time. Each simulation run begins from a cold start, where each joining node knows only one existing node in the system and must discover other nodes in the system through the gossip protocol.

We first evaluate how accurate Meridian is in finding the closest node to a given target compared to the embedding based approaches. We computed the coordinates for our 2500 node data set using GNP, Vivaldi and Vivaldi with height. GNP represents an absolute coordinate scheme based on static landmarks. We configured it for 15 landmarks and 8 dimensions as suggested by the GNP authors, and used the N-clustered-medians protocol for landmark selection. Vivaldi is another absolute coordinate scheme based on spring simulations and was configured to use 6 dimensions with 32 neighbors. Vivaldi with height is a recent scheme that performs a non-Euclidian embedding which assigns a 2 dimensional location plus a height value to each node. We randomly select 500 targets from our data set of 2500 nodes.

We first examine the inherent embedding error in absolute coordinate systems and determine the error involved in selecting the closest nodes. The darker bars in Figure 5 show the median embedding error of each of the coordinate schemes, where the embedding error is the absolute value of the difference between the measured distance and predicted distance over all node pairs. However, even with a large embedding error, it is possible for the coordinate systems to pick the correct closest node. To evaluate this, we assumed the presence of a perfect geographic query routing layer, such as an actual CAN deployment with perfect information at each node. This assumption biases the experiment towards virtual coordinate systems and isolates the error inherent in network embeddings. The median closest node discovery error for all three embedding schemes, as shown by the lighter bars in Figure 5, are an order of magnitude higher than Meridian. Figure 6 compares the relative error CDFs of different closest node discovery schemes. Meridian has a lower relative error than the embedding schemes by a large margin over the entire distribution.

The accuracy of our closest node discovery protocol depends on several parameters of our system, such as the number of nodes per ring k, acceptance interval β, the constant β_0, and the gossip rate. The most critical parameter is the number of nodes per ring k, as this determines the granularity of the search where a higher number of nodes per ring will comb through the search space at a finer grain. Figure 7 shows the median error drops sharply as k increases. This is significant as a node only needs to keep track of a small number of other nodes to achieve high accuracy. The results indicate that as few as eight nodes per ring can return very accurate results with a system size of 2000 nodes. As each node only has nine total rings, a node must only be aware of at most seventy-two other nodes in the system.

High accuracy must also be coupled with low query latency for interactive applications that have a short lifetime per query and cannot tolerate a long initial setup time. The closest node discovery latency is dominated by the sum of the maximum latency probe at each hop plus the node to node forwarding latency; we ignore processing overheads because they are negligible in comparison. Meridian bounds the maximum latency probe by two times the latency from the current intermediate node to the destination, as any probe that requires more time cannot be a closer node and its result is discarded. The average query latency curve in Figure 7 shows that queries are resolved quickly regardless of k. Average query latency is determined by the slowest node in each ring (subject to the maximum latency bound) and the hop count, both of which increases only marginally as k increases from four to sixteen.

The β parameter captures the tradeoff between query latency and accuracy as shown in Figure 8. Increasing β increases the query latency, as it reduces the improvements necessary before taking a hop, and therefore increases the number of hops taken in a query. However, increasing β also provides a significant increase in accuracy for β < 0.5; this matches our analysis (see Thm. 4.2, and also Thm. 4.3 and Thm. 4.4a). Accuracy is not sensitive to β for β > 0.5.

We examine the scalability of the closest node discovery application by evaluating the error, latency and aggregate load at different system sizes. Figure 9 plots the median error and average query latency for k = log N. As predicted by the theoretical analysis in Section 4, the median error remains constant as the network grows, varying only within the error margin. The error improves for really small networks where it is feasible to test all possible nodes for proximity. Similarly, the query latency remains constant for all tested system sizes.

Scalability also depends on the aggregate load the system places on the network, as this can limit the number of concurrent closest node discoveries that can be performed at a particular system size. Figure 10 plots the total bandwidth required throughout the entire network to resolve a query, and shows that it grows sub-linearly with system size, with 2000 nodes requiring a total of 2.6 KB per query.

A desirable property for load-balancing, and one of the assumptions in our theoretical analysis (see Thm. 4.4b on load-balancing) is stochastic independence of the ring sets. We verify this property indirectly by measuring the in-degree ratio of the nodes in the system. The in-degree ratio is defined as the number of incoming links to a node A over the average number of incoming links to nodes within a ball of radius r around A. If the ring sets are independent then the in-degree ratio should be close to 1; in other words, it would indicate that the nodes within the radius r around A are selected evenly as neighbors. Figure 11 shows that Meridian is very evenly load-balanced, as more than 90% of the balls have an in-degree ratio less than two for balls of radius 20ms and 50ms.

A desirable property, and one of the assumptions in our theoretical analysis (see Thm. 4.2) is that our ring members are well distributed due to our multi-resolution ring structure and our hypervolume ring membership replacement scheme. To determine their actual effectiveness, we evaluate the latency ratio of the nodes. The latency ratio for a node A and a target node B is defined as the latency of node C to B over the latency of A to B, where C is the neighbor of A that is closest to B. The CDF in Figure 12 indicates that for β = 0.5, Meridian's ring member selection algorithm can make progress via an extra hop to a closer node more than 80% of the time. For β = 0.75, an extra hop can be taken over 97% of the time. This gives a good indication that multi-resolution rings and hypervolume ring membership replacement protocol are doing a good job in distributing the ring nodes in the latency space. The hypervolume ring membership protocol also provides significantly more consistent results than a random replacement protocol, as the standard deviation of relative error is 38ms using hypervolume replacement, but is 151ms when using random replacement.

We evaluate how Meridian performs in central leader election by measuring its relative error as a function of group size. Figure 13 shows that, as group size gets larger, the relative error of the central leader election application drops. Intuitively, this is because the larger group sizes increase the number of nodes eligible to serve as a well-situated leader, and simplify Meridian's task of routing the query to a suitable node.

We evaluate our multi-constraint protocol by the percentage of queries that it can satisfy, parameterized by the difficulty of the set of constraints. For each multi-constraint query we select four random target nodes, and the constraint to each target node is drawn from a uniform distribution between 40 and 80 ms. The difficulty of a set of constraints is determined by the number of nodes in the system that can satisfy them. The fewer the nodes that can satisfy the set of constraints, the more difficult is the query.

Figure 14 shows a histogram of the success rate broken down by the percentage of nodes in the system that can satisfy the set of constraints. For queries that can be satisfied by 0.5% of the nodes in the system or more, the success rate is over 90%.

As in closest node discovery, the number of nodes per ring k has the largest influence on the performance of the multi-constraint protocol. Figure 15 shows the failure rate decreases as the number of nodes per ring increases. Surprisingly, it also shows a decrease in average query latency as the number of nodes per ring increases. This is due to the reduction in the number of hops needed before a constraint satisfying node is found, as a search can end early by finding a satisfactory node. Figure 16 shows that varying β in the multi-constraint protocol has similar but less pronounced effects as in the closest node discovery protocol. An increase in β decreases the failure percentage and increases the average latency of a multi-constraint query. Overall, the effect of β on application performance is small for multi-constraint node selection.

The scalability properties of the multi-constraint system are very similar to the scalability of closest node discovery. Figure 17 shows that the failure rate and the average query latency are independent of system size, even when the number of nodes per ring k = log N (note that setting k to a constant would favor the runs with small N). Figure 18 shows that the average load per multi-constraint query grows sub-linearly. The non-increasing failure rate and the sub-linear growth of the query load make the multi-constraint protocol highly scalable.

### Physical Deployment

We have implemented and deployed the Meridian framework and the closest node discovery protocol on PlanetLab. The implementation is small, compact and straightforward; it consists of approximately 2500 lines of C++ code. Most of the complexity stems from support for firewalled hosts.

Hosts behind firewalls and NATs are very common on the Internet, and a system must support them if it expects large-scale deployment over uncontrolled, heterogeneous hosts. Meridian supports such hosts by pairing each firewalled host with a fully accessible peer, and connecting the pair via a persistent TCP connection. Messages bound for the firewalled host are routed through its fully accessible peer — a ping, which would ordinarily be sent as a direct UDP packet or a TCP connect request, is sent to the proxy node instead, which forwards it to the destination, which then performs the ping to the originating node and reports the result. A node whose proxy fails is considered to have failed, and must join the network from scratch to acquire a new proxy. Since a firewalled host cannot directly or indirectly ping another firewalled host, firewalled hosts are excluded from ring membership on other firewalled hosts, but included on fully-accessible nodes.

We deployed the Meridian implementation over 166 PlanetLab nodes. We benchmark the system with 1600 target web servers drawn randomly from the Yahoo web directory, and examine the latency to the target from the node selected by Meridian versus the optimal obtained by querying every node. Meridian was configured with k = 8, α = 2 ms, β = 0.5, and β_0 = 1. Overall, median error in Meridian is 1.844ms, and the relative error CDF in Figure 19 shows that it performs better than simulation results from a similarly configured system.

---

## 6. Related Work

Meridian is a general node proximity framework that we have applied to server selection. We separate the server selection techniques into those that require network embedding and those that do not, and survey both in turn.

### Network Embedding

Recent work on network coordinates can be categorized roughly into landmark based systems, and the simulation based systems. Both types can embed nodes into a Euclidean coordinate space. Such an embedding allows the distance between any two nodes to be determined without direct measurement.

GNP is the pioneer in network embedding systems. It uses a fixed set of landmarks that determines the coordinates of a node by its distance to the landmarks. ICS and Virtual Landmarks both aim to reduce the computational cost of GNP by replacing the embedding with ones that are computationally cheaper, at the cost of losing accuracy. Meridian uses the same low-cost embedding as Virtual Landmarks, but employs the resulting coordinates only for selecting diverse ring members, not for resolving queries. To address the issue of single point of failure due to fixed landmarks, Lighthouse uses multiple local coordinate systems that are joined together through a transition matrix to form a global coordinate system. PIC and PCoord only require fixed landmarks for bootstrapping and calculate their coordinates based on the coordinates of peers. This can lead to compounding of embedding errors over time in a system with churn. NPS is similar to PIC and PCoord but further imposes a hierarchy of servers to ensure consistency of the coordinates across all the nodes. Vivaldi is based on a simulation of springs, where the position of the nodes that minimizes the potential energy of the spring also minimizes the embedding error. BBS performs a similar simulation to calculate coordinates, simulating an explosion of particles under a force field.

IDMaps, like network embedding systems, can compute the approximate distance between two IP addresses without direct measurement based on strategically placed tracer nodes. IDMaps incurs inherent errors based on the client's distance to its closest tracer server and requires deploying system wide infrastructure. Other work has also examined how to delegate probing to specialized nodes in the network.

There has also been theoretical work on explaining the empirical success of network embeddings and IDMaps-style approaches.

### Server Selection

Our closest node discovery protocol draws its inspiration from DHTs such as Chord, Pastry and Tapestry, but these DHTs solve a different problem, namely routing. Proximity based neighbor selection performs a similar search using the node entries in the route table of a structured P2P system. This technique relies on the routing table levels to loosely characterize peer nodes by latency, but does not directly organize nodes based on their latency and incurs the overhead associated with structured P2P systems. The time and space complexity of two similar techniques are discussed in related work, but these techniques do not provide a general framework, and instead focus exclusively on finding the nearest neighbor. Moreover, their results appear to apply only to Internet latencies modeled by growth-constrained metrics, whereas our framework extends to a more general model (see Section 4). Also, without an evaluation on a large scale data set collected from live Internet nodes, their practicality cannot be confirmed.

A closest node discovery technique described as "binning" is introduced in related work, where m landmarks are placed, each keeping track of its latency to the nodes in the system. A node finds the closest node by querying all m landmarks for nodes that are the same distance d away from the landmarks, and choosing the closest node from that set. The accuracy of the system depends heavily on the assumption that triangle inequality holds on the majority of routes, and the choice of an appropriate d is not obvious without prior knowledge of the node distribution.

Another landmark based technique for closest node discovery is described in related work, where each node determines its bin number via measurements to well known landmarks. A node wishing to find its closest node determines its own bin number, queries a modified DNS server for other nodes in the same bin, or the nearest bin if no other nodes belong in the same bin, and chooses a random node from the retrieved set of servers.

Several different proactive techniques to locate the closest replica to the client are evaluated in related work. These techniques offer different methods to construct a connectivity graph by means of polling the routing table of connecting hops, explicitly sending routing probes, or limited probing with triangulation. The study assumes the network conditions and topology remain relatively static, and does not directly address scalability.

Dynamic server selection was found in related work to be more effective than static server selection due to the variability of route latency over time and the large divergence between hop count and latency. Simulations using a simple dynamic server selection policy, where all replica servers are probed and the server with the lowest average latency is selected, show the positive system wide effects of latency-based server selection. Our closest node discovery application can be used to perform such a selection in large-scale networks.

---

## 7. Conclusions

Network positioning based node selection is a critical building block for many large scale distributed applications. Network coordinate systems, coupled with a scalable node selection substrate, may provide one possible approach to solving such problems. However, the generality of absolute coordinate systems comes at the expense of accuracy and complexity.

In this paper, we outlined a lightweight, accurate and scalable framework for solving positioning problems without the use of explicit network coordinates. Our approach is based on a loosely structured overlay network and uses direct measurements instead of virtual coordinates to perform location-aware query routing without incurring either the complexity, overhead or inaccuracy of an embedding into an absolute coordinate system or the complexity of a geographic peer-to-peer routing substrate such as CAN and P-Trees.

We have argued analytically that Meridian provides robust performance, delivers high scalability, and balances load evenly across nodes. We have evaluated our system through a PlanetLab deployment as well as extensive simulations, parameterized by data from measurements of 2500 nodes and 6.25 million node pairs. The evaluation indicates that Meridian is effective; it incurs less error than systems based on an absolute embedding, is decentralized, requires relatively modest state and processing, and locates nodes quickly. We have shown how the framework can be used to solve three network positioning problems frequently-encountered in distributed systems; it remains to be seen whether the lightweight approach advocated in this paper can be applied to other significant problems.

---

## Acknowledgments

The authors are grateful to Frank Dabek, Russ Cox, Frans Kaashoek, Robert Morris, Eugene Ng and Hui Zhang for sharing the Vivaldi and GNP software and data.

---

## References

[1] D. Andersen, H. Balakrishnan, M. Kaashoek, and R. Morris. Resilient Overlay Networks. In SOSP 2001.

[2] A. Bavier, M. Bowman, B. Chun, D. Culler, S. Karlin, S. Muir, L. Peterson, T. Roscoe, T. Spalink, and M. Wawrzoniak. Operating System Support for Planetary-Scale Network Services. In NSDI 2004.

[3] R. Carter and M. Crovella. Server Selection Using Dynamic Path Characterization in Wide-Area Networks. In INFOCOM 1997.

[4] R. Carter and M. Crovella. On the Network Impact of Dynamic Server Selection. Computer Networks, 31, 1999.

[5] M. Castro, P. Druschel, Y. Hu, and A. Rowstron. Exploiting network proximity in peer-to-peer overlay networks. Tech Report MSR-TR-2003-82, Microsoft Research, 2002.

[6] M. Castro, P. Druschel, Y. Hu, and A. Rowstron. Proximity neighbor selection in tree-based structured peer-to-peer overlays. Tech Report MSR-TR-2003-52, MSR, 2003.

[7] U. G. Center. QHull. UIUC Geometry Center, QHull Computational Geometry Package, http://www.qhull.org, 2004.

[8] Y. Chu, S. Rao, and H. Zhang. A Case for End System Multicast. In SIGMETRICS 2000.

[9] M. Costa, M. Castro, A. Rowstron, and P. Key. PIC: Practical Internet Coordinates for Distance Estimation. In ICDCS 2004.

[10] A. Crainiceanu, P. Linga, J. Gehrke, and J. Shanmugasundaram. Querying Peer-to-Peer Networks Using P-Trees. In WebDB 2004.

[11] W. Cui, I. Stoica, and R. Katz. Backup Path Allocation Based On A Correlated Link Failure Probability Model In Overlay Networks. In ICNP 2002.

[12] F. Dabek, R. Cox, F. Kaashoek, and R. Morris. Vivaldi: A Decentralized Network Coordinate System. In SIGCOMM 2004.

[13] F. Dabek, M. Kaashoek, D. Karger, R. Morris, and I. Stoica. Wide-area cooperative storage with CFS. In SOSP 2001.

[14] A. Demers, D. Greene, C. Hauser, W. Irish, J. Larson, S. Shenker, H. Sturgis, D. Swinehart, and D. Terry. Epidemic algorithms for replicated database maintenance. In PODC 1987.

[15] Z. Fei, S. Bhattacharjee, E. Zegura, and M. Ammar. A Novel Server Selection Technique for Improving the Response Time of a Replicated Service. In INFOCOM 1998.

[16] P. Francis, S. Jamin, C. Jin, Y. Jin, D. Raz, Y. Shavitt, and L. Zhang. IDMaps: A global Internet host distance estimation service. IEEE/ACM Transactions on Networking, 9:525–540, October 2001.

[17] K. Gummadi, S. Saroiu, and S. Gribble. King: Estimating Latency between Arbitrary Internet End Hosts. In IMW 2002.

[18] A. Gupta, R. Krauthgamer, and J. Lee. Bounded geometries, fractals, and low-distortion embeddings. In FOCS 2003.

[19] J. Guyton and M. Schwartz. Locating Nearby Copies of Replicated Internet Servers. In SIGCOMM 1995.

[20] J. Heinonen. Lectures on analysis on metric spaces. Springer Verlag, Universitext 2001.

[21] K. Hildrum, R. Krauthgamer, and J. Kubiatowicz. Object location in realistic network. In SPAA 2004.

[22] K. Hildrum, J. Kubiatowicz, S. Ma, and S. Rao. A note on finding the nearest neighbor in growth-restricted metrics. In SODA 2004.

[23] K. Hildrum, J. Kubiatowicz, and S. Rao. Another way to find the nearest neighbor in growth-restricted Metrics. UC Berkeley CSD ETR, UCB/CSD-03-1267, UC Berkeley, August 2003.

[24] K. Hildrum, J. Kubiatowicz, S. Rao, and B. Zhao. Distributed Object Location in a Dynamic Network. In SPAA 2002.

[25] K. Johnson, J. Carr, M. Day, and M. Kaashoek. The measured performance of content distribution networks. In WCW 2000.

[26] D. Karger and M. Ruhl. Finding Nearest Neighbors in Growth-restricted Metrics. In STOC 2002.

[27] D. Kempe, J. Kleinberg, and A. Demers. Spatial Gossip and Resource Location Protocols. In STOC 2001.

[28] J. Kleinberg, A. Slivkins, and T. Wexler. Triangulation and embedding using small sets of beacons. In FOCS 2004.

[29] C. Kommareddy, N. Shankar, and B. Bhattacharjee. Finding Close Friends on the Internet. In ICNP 2001.

[30] R. Krauthgamer and J. Lee. Navigating nets: simple algorithms for proximity search. In SODA 2004.

[31] R. Krauthgamer and J. Lee. The black-box complexity of nearest neighbor search. In ICALP 2004.

[32] R. Krauthgamer and J. Lee. The intrinsic dimensionality of graphs. In STOC 2003.

[33] R. Krauthgamer, J. Lee, M. Mendel, and A. Naor. Measured descent: a new embedding method for finite metrics. In FOCS 2004.

[34] R. Lawrence. Running Massively Multiplayer Games as a Business. Keynote: NSDI 2004.

[35] L. Lehman and S. Lerman. PCoord: Network Position Estimation Using Peer-to-Peer Measurements. In NCA 2004.

[36] H. Lim, J. Hou, and C. Choi. Constructing Internet Coordinate System Based on Delay Measurement. In IMC 2003.

[37] P. Maniatis, M. Roussopoulos, T. Giuli, D. Rosenthal, M. Baker, and Y. Muliadi. Preserving peer replicas by rate-limited sampled voting. In SOSP 2003.

[38] R. Motwani and P. Raghavan. Randomized algorithms. Cambridge University Press, 1995.

[39] T. Ng and H. Zhang. A Network Positioning System for the Internet. In USENIX 2004.

[40] T. Ng and H. Zhang. Predicting Internet Network Distance with Coordinates-Based Approaches. In INFOCOM 2002.

[41] M. Pias, J. Crowcroft, S. Wilbur, T. Harris, and S. Bhatti. Lighthouses for Scalable Distributed Location. In IPTPS 2003.

[42] C. Plaxton, R. Rajaraman, and A. Richa. Accessing nearby copies of replicated objects in a distributed environment. In SPAA 1997.

[43] S. Ratnasamy, P. Francis, M. Hadley, R. Karp, and S. Shenker. A Scalable Content-Addressable Network. In SIGCOMM 2001.

[44] S. Ratnasamy, M. Handley, R. Karp, and S. Shenker. Topologically-Aware Overlay Construction and Server Selection. In INFOCOM 2002.

[45] A. Rowstron and P. Druschel. Pastry: Scalable, distributed object location and routing for large-scale peer-to-peer systems. In Middleware 2001.

[46] A. Rowstron and P. Druschel. Storage management and caching in PAST, a large-scale, persistent peer-to-peer storage utility. In SOSP 2001.

[47] S. Savage, A. Collins, and E. Hoffman. The End-to-End Effects of Internet Path Selection. In SIGCOMM 1999.

[48] Y. Shavitt and T. Tankel. Big-Bang Simulation for Embedding Network Distances in Euclidean Space. In INFOCOM 2003.

[49] A. Slivkins. Distributed approaches to triangulation and embedding. In SODA 2005.

[50] I. Stoica, R. Morris, D. Karger, F. Kaashoek, and H. Balakrishnan. Chord: A Scalable Peer-to-peer Lookup Service for Internet Applications. In ACM SIGCOMM 2001.

[51] K. Talwar. Bypassing the embedding: approximation schemes and compact representations for growth restricted metrics. In STOC 2004.

[52] L. Tang and M. Crovella. Virtual Landmarks for the Internet. In IMC 2003.

[53] H. Weatherspoon, T. Moscovitz, and J. Kubiatowicz. Introspective Failure Analysis: Avoiding Correlated Failures in Peer-to-Peer Systems. In RPPDS 2002.

[54] B. Zhao, J. Kubiatowicz, and A. Joseph. Tapestry: An infrastructure for fault-tolerant wide-area location and routing. Technical Report UCB/CSD-01-1141, UC Berkeley, April 2001.

---

## Appendix A: Proofs

Here we provide proofs for the results in Section 4. First we address the quality of the rings, then approximate nearest neighbors, then exact nearest neighbors, and conclude with load-balancing; the proof on load-balancing is significantly more complicated than the other proofs. To make this write-up self-contained, we include a subsection on Chernoff Bounds that are used throughout the proofs.

For simplicity we redefine the KR-dimension as the smallest D such that for any c > 0 the cardinality of any ball B_A(r) is at most c^D times larger than that of B_A(r/c). It is easy to check that this definition coincides with the old one for any c = 2^i, i ≥ 1. We redefine the doubling dimension similarly.

If C is a neighbor of the current node A, and T is the target, then call C a progress-c neighbor if d_{A,C}/d_{B,C} ≥ c.

### A.1 Quality of the Rings

We start with two proofs which show that even with small k it is possible to make the rings ε-nice.

**Proof of Thm. 4.1:** Fix two Meridian nodes A,B and let r = ε·d_{A,B}. Pick the smallest i such that d_{A,B} + r < β·α^i. Then B_{A,i} ⊆ B_B(β·α^i + d_{A,B}) ⊆ B_B(β·α^i + r - r) = B_B(γ·r), where γ = (β·α^i)/r - 1/ε, so |B_{A,i}| ≤ γ^D·|B_B(r)|. Therefore by Chernoff Bounds (Claim A.10) some node from R_{A,i} lands in B_B(r) with failure probability at most δ/N^2. ∎

**Proof of Thm. 4.5:** Fix two Meridian nodes A,B and let r = ε·d_{A,B}. Pick the smallest i such that d_{A,B} + r < β·α^i. Then r > β·α^i·γ, where γ = ε/(1+ε). So applying the definition of a doubling measure log_α(1/γ) times we see that μ(B_{A,i})/μ(B_B(r)) ≤ 2^(DIM·log_α(1/γ)) = γ^(DIM/log_α(2)). The ring R_{A,i} is distributed as in the following process: pick nodes from B_{A,i} independently with probability μ(x)/μ(B_{A,i}), until we gather k distinct nodes. At each draw the probability of choosing a node from B_B(r) is at least γ^(DIM/log_α(2)). The claim follows from the Chernoff Bounds (Lemma A.9), exactly as in Claim A.10. ∎

### A.2 Approximate Nearest Neighbor

In this subsection we prove Thm. 4.2.

The search algorithm used by Meridian (denoted by A(c) in Section 4) looks only at three rings at a given node. For Thm. 4.2 we'll need a generalization A(c, l), which looks at l ≥ 3 rings. Specifically, if node A receives a query for target T, it chooses i = 1 + ⌊log_α d_{A,T}⌋, and finds a neighbor C in the l rings R_{A,j}, i - ⌊l/2⌋ ≤ j ≤ i + ⌊l/2⌋ that is closest to T. If C is a progress-c neighbor then the query is forwarded to C; else the search stops.

The following claim essentially shows that if we look at the ring of radius that is too small then we cannot make much progress towards the target node.

**Claim A.1:** For any nodes (A, C, T), suppose d_{A,C} < β·α^(i-1), i = ⌊log_α d_{A,T}⌋. Then d_{A,T}/d_{C,T} ≤ 1 + 2^(1-i).

*Proof:* Let γ = 2^(1-i). Then d_{A,C} < γ·β·α^(i-1) ≤ γ·d_{A,T}, so d_{C,T} ≥ d_{A,T} - d_{A,C} > (1-γ)·d_{A,T}, and it follows that d_{A,T}/d_{C,T} < 1/(1-γ) ≤ 1+2γ. ∎

By Claim A.1 in a given step we might not need to look at all l rings: we look at the rings R_{A,j} in the order of decreasing j, and without loss of generality we consider the ring R_{A,j}, j < i - 1 only if in the larger rings there was no node B such that d_{A,T}/d_{B,T} ≥ 1+2^(j-i+2). In particular, if for some l algorithm A(c, l) finds a progress-2 node then so does A(c, 3).

The following claim shows how our algorithm A zooms in on the target node. We'll use the function:

```
f(c) = c·(1+ε)/(1 - c·ε)
```

Note that for c ∈ (1, 1/ε) the function f(c) is continuously increasing to infinity. Define l(c) = 3 if c ≥ 2 and l(c) = 3 + ⌊log_α(1/(2-c))⌋ otherwise.

**Claim A.2:** Assume the rings are ε-nice. Let T be the target node, let B ∈ M be its nearest neighbor. Let A be any Meridian node, suppose d_{A,T}/d_{B,T} = f(c) for some c ∈ (1, 1/ε), and fix l ≥ l(c). Then at A algorithm A(c, l) finds a progress-c neighbor of A.

*Proof:* First we claim that such neighbor exists. Indeed, pick the smallest i such that d_{A,B}·(1+ε) < β·α^i. Since the rings are ε-nice, node A has a neighbor C ∈ R_{A,i} within distance ε·d_{A,B} from node B. Then, letting d = d_{B,T}:

d_{C,T} ≤ d + d_{B,C} < d + ε·d_{A,B} ≤ d + ε·(d + d_{A,T}) = ε·d_{A,T} + (1+ε)·d_{A,T}/f(c) = d_{A,T}/c,

claim proved. It remains to show that C lies in one of the rings considered by the algorithm A(l), i.e. that if d_{A,T} < β·α^j ≤ 2·d_{A,T} then j - ⌊l/2⌋ ≤ i ≤ j + ⌊l/2⌋. Indeed, i ≤ j+1 follows since d_{A,B} < d_{A,T} + d < d_{A,T}·(1 + 1/f(c)) < d_{A,T}·(1 + 1/f(1)) = 2·d_{A,T}/(1+ε), β·α^i ≤ 2·d_{A,T} ≤ β·α^(j+1), and i ≥ j-⌊l/2⌋ follows by Claim A.1. ∎

The next claim allows us to use Claim A.2 for small c; the proof is straightforward.

**Claim A.3:** For any γ ∈ (0, 1) we have f(1+γ)/(1+γ) < f(1+γ/2). Moreover, f(1+ε/2) < 1+3ε.

Now we are ready to prove Thm. 4.2.

**Proof of Thm. 4.2:** Let T be the target of the nearest-neighbor query, and let d be the distance from T to its closest Meridian neighbor.

(a) We need to prove that algorithm A(2, 3) finds a 3-approximate neighbor of T. By Claim A.2 while the query visits nodes A such that d_{A,T}/d ≥ f(2), the algorithm finds a progress-2 neighbor of A and forwards the query to it. The distance to T goes down by a factor of at least 2 at each step, so after at most log_α(R) steps the query should arrive at some node B such that d_{B,T}/d < f(2) < 3. This proves part (a).

(b) We'll show that A(c, l) finds a (1+3ε)-approximate neighbor of T, where l = 3 + ⌊log_α(2/ε)⌋. By Claim A.2 while the query visits nodes A such that d_{A,T}/d ≥ f(2), the distance to T goes down by a factor of at least 2 at each step. So after at most log_α(R) steps the query should arrive at some node B such that d_{B,T}/d < f(2). Then by Claims A.2 and A.3, using induction on j we show that after j ≤ l more steps the query will arrive at node C such that d_{C,T}/d < f(1+2^(-j)). In particular, by Claim A.3 we are done when j = log_α(2/ε). This proves part (b).

(c) Note that if any neighbor of the current node A achieves progress less than 1+γ, γ ∈ (0, 1/2) then by Claim A.2 we have d_{A,T}/d < f(1+γ) < 1+ε+2γ. ∎

### A.3 Exact Nearest Neighbor

Here we prove Thms. 4.3 and 4.4a on finding exact nearest neighbors. In both theorems the progress is at least 2 at every step except maybe the last one; we have to be careful about this last step, since in general the target is not a Meridian node and therefore not a member of any ring.

**Proof of Thm. 4.3:** Let k = 2·10^D·log(N|X|/δ). Let T ∈ X be the target, and let B ∈ M be its exact nearest neighbor. Fix some Meridian node A, let d = d_{A,T} and choose i such that 2d < β·α^i ≤ 4d.

We claim that either B ∈ R_{A,i}, or with failure probability at most δ/(N|X|·log_α(R)) the ring R_{A,i} contains some C ∈ B_B(d/2). Indeed, B_{A,i} ⊆ B_T(4d), so |B_{A,i}| ≤ |B_T(4d)| ≤ 10^D·|B_T(d/2)|, so if |B_{A,i}| ≥ k then the claim follows from Claim A.10; the constant 2·10^D in front of k works numerically as long as e.g. N|X| > 5000 and δ > 10^(-6), which is quite reasonable. Finally, if |B_{A,i}| < k then every node in B_{A,i} is in ring R_{A,i}, including B, claim proved.

So the progress is at least 2 at every step except maybe the last one, with failure probability at most δ/(N|X|). The Union Bound over all N|X| possible A,T pairs gives the total failure probability δ. ∎

Thm. 4.4a is proved using the same idea, except we need to address the fact that Meridian nodes themselves are chosen at random from X. Let X_A(r) denote the closed ball in X of radius r around node A, i.e. the set of all nodes in X within distance r from A.

**Proof of Thm. 4.4a:** Denote X_{A,i} = X_A(β·α^i) and let k = 8·10^D·log(2N|X|/δ). Let T be the target and let B ∈ M be its exact nearest neighbor. Fix some Meridian node A, let d = d_{A,T} and S = B_T(d/2), and choose i such that 2d < β·α^i ≤ 4d.

Note that we can view the process of selecting M from X as follows: choose the cardinality m for B_{A,i} from the appropriate distribution, then choose, independently and uniformly at random, m nodes from X_{A,i}, and N-m nodes from X\X_{A,i}.

We claim that with failure probability at most δ' = δ/(N|X|) either B ∈ R_{A,i}, or the ring R_{A,i} contains some C ∈ S. Indeed, if the cardinality of B_{A,i} is at most k, then all of B_{A,i} lies in the ring R_{A,i}, including B. Now assume the cardinality of B_{A,i} is some fixed number m > k. Since X_{A,i} ⊆ X_T(4d), it follows that m/E[|S|] = |X_{A,i}|/|X_T(d/2)| ≤ |X_T(4d)|/|X_T(d/2)| ≤ 10^D, so by Claim A.11a with failure probability at most δ'/2 the cardinality of S is at least half the expectation, so that by Claim A.10 with failure probability at most δ'/2 some node in ring R_{A,i} lands in S. Claim proved.

Therefore the progress is at least 2 at every step except maybe the last one, with failure probability at most δ'. The Union Bound over all N|U| possible A,T pairs gives the total failure probability δ. ∎

### A.4 Load-Balancing: Thm. 4.4b

In this subsection we'll prove Thm. 4.4b, which is about load-balancing. A large part of the proof is the setup: it is non-trivial to restate the algorithm and define the random variables so that the forth-coming Chernoff Bounds-based argument works through. For convenience, for any m ≥ 0 denote [m] = {0, 1, ..., m-1}.

For technical reason we'll need a slightly modified search algorithm. On every step in algorithms A(·) and A'(·) we look at a subset S of neighbors, and either the search stops, or the query is forwarded to the node C ∈ S that is closest to the target. Here is the modification: if C is a progress-2 node, then instead of forwarding to C the algorithm can forward the query to an arbitrary progress-2 node in S. It is easy to check that all our results for A(·) and A'(·) carry over to this modification. For Thm. 4.4b we'll need a specific version of A'(2) which can be seen as a rule to select between different progress-2 nodes.

As compared to (the proof of) part (a), we increase the ring cardinalities by a factor of O(log N)·O(log R). This is essentially because we need more randomness, so that in the proof we could use Chernoff Bounds more efficiently. While it might be possible to prove the theorem without this blow-up, it seems to lead to mathematical difficulties that are way beyond the scope of this paper.

Recall that R_{A,i} is well-formed if it is distributed as a random k-node subset of B_{A,i}. Here for technical convenience we'll use a slightly different definition: say R_{A,i} is well-formed if it is distributed as a set of k nodes drawn independently uniformly at random from B_{A,i}. The difference is that the new definition allows repetitions; note that all previous results work under either definition.

Let's define our version of A'(2), which we denote A*. Say each ring R_{A,i} consists of k slots S_{A,i}(j) which can be seen as independent random variables distributed uniformly at random in B_{A,i}. Since the rings are independent, {S_{A,i}(j) : A ∈ M, i ∈ [log_α R], j ∈ [k]} is a family of independent random variables.

Let L = 6·log(2N·log_α(R)/δ). For every pair A,i, partition the slots S_{A,i}(·) into L·log_α(R) equal-size collections C_{A,i}(t, u), where t ∈ [log_α R] and u ∈ [L]; formally, each such collection is a set of indices j' into S_{A,i}(j'). Let R_{A,i}(t, u) = {S_{A,i}(j') : j' ∈ C_{A,i}(t, u)} ⊆ B_{A,i} be the set of values of slots in C_{A,i}(t, u). Obviously, the union of all sets R_{A,i}(·, ·) is R_{A,i}.

Say a t-step query is a query on the t-th step of the algorithm. When node A receives a t-step query to target T, it chooses u ∈ [L] in a round-robin fashion (the round-robin is separate for each A,t pair) and lets algorithm A(1) handle this query using only the neighbors in R_{A,i}(t, u), for the corresponding i. Specifically, A(1) sets i = 1 + ⌊log_α d_{A,T}⌋, asks every node in R_{A,i}(t, u) to measure the distance to T, and forwards the query to the closest one.

We make each collection C_{A,i}(t, u) have size k' = 8·10^D·log(2M/δ), where M = N|X|·L·log_α(R). Then using the argument from part (a) we can show that for given (A, T, t, u) either the corresponding R_{A,i}(t, u) contains a progress-2 node or it contains a nearest neighbor of T, with failure probability at most δ/M. The Union Bound over all M possible (A, T, t, u) tuples shows that our algorithm is X-exact with failure probability δ.

Note that algorithm A* can be seen as A'(·) with a rule to select between different progress-2 nodes: namely, choose a progress-2 node from the corresponding R_{A,i}(t, u) if such node exists, else proceed with A'(·). Obviously, under assumptions of Thm. 4.4a with failure probability δ this scheme will behave exactly as A*.

We consider a scenario where many independent random choices are made. Specifically we choose:

- N-node subset M of X,
- subsets R_{A,i}(t, u) ⊆ B_{A,i} for each tuple (A, i, t, u),
- target T_A for each node A.

For a collection of independent random choices, without loss of generality we can assume that a given choice happens any time before its result is actually used. In particular, we will assume that at first, M and T_A's are chosen. Then the time proceeds in log_α(R) epochs; in a given epoch t, all subsets R_{A,i}(t, ·) are chosen, then all queries are advanced for one step.

Let's analyze the choice of M and the queries. Let Q be the set of all N queries. For q ∈ Q, let T(q) be the corresponding target. Let Q_B(r) be the set of queries q ∈ Q such that T(q) is within distance r from B. Let T(S) be the set of all targets in the set S of queries. Let γ = N/|X|. By Claim A.11 |B_A(r)| and |Q_A(r)| are close to its expectation:

**Claim A.4:** With failure probability at most δ, for any A ∈ M ∪ T(Q) and radius r the following holds: (*) if μ = γ·|X_A(r)| ≥ k' then |B_A(r)| and |Q_A(r)| are within a factor of 2 from μ, else they are at most 2k', where k' = O(log(N/δ)).

This completes the setup; now, finally, we can argue about load-balancing. We need some more preliminaries.

Recall that all queries are handled separately, even if a given node simultaneously receives multiple queries for the same target. When node A handles a t-step query and in the process measures distance to its neighbor B, we say that B receives a t-step request from A. Let's define several families of random variables:

- F_{A,B}(t, u) is the number of t-step queries forwarded from A to B, and handled at A using a set R_{A,i}(t, u), for some i.
- F^t_A is the number of all t-step queries forwarded to node A; set F^0_A = 1. Then F_{A,B}(t, u) ≤ F^t_A/L.
- G_{A,B}(t, u) is the number of t-step requests received by B from A, and handled at A using a set R_{A,i}(t, u), for some i.
- G^t_B is the number of all t-step requests received by node B. Clearly, G_{A,B}(t, u) ≤ F^(t-1)_A/L.

For every t-step query received, a given node sends some constant number p of packets to each of the k' neighbors in the corresponding set R_{A,i}(t, u). Therefore a given node A sends p·k'·Σ_t F^t_A packets total, and receives p·Σ_t G^t_A packets total. Since a single query involves exchanging at most p·k'·log_α(R) packets, algorithm A* is (β, X)-balanced if and only if Σ_t (k'·F^t_A + G^t_A) < 2β·k'·log_α(R) for every node A.

Say property P(t) holds if for each node B it is the case that F^t_B < β and G^t_B/k' < β. We need to prove that with high probability P(t) holds for all t. It suffices to prove the following claim:

**Claim A.5:** If P(t-1) then P(t), with failure probability at most δ/log_α(R).

Then we can take the Union Bound over all log_α R steps to achieve the desired failure probability δ.

Let's prove Claim A.5. Suppose all queries have completed t-1 steps and are assigned to the respective sets R_{A,i}(t, u). Now the only remaining source of randomness before the t-th step is the choice of these sets. In particular, each random variable F_{A,B}(t, u) depends only on one set R_{A,i}(t, u), and so does G_{A,B}(t, u). Since these sets are chosen independently, for any fixed B variables {F_{A,B}(t, u) : A ∈ M, u ∈ [L]} are independent, and so are {G_{A,B}(t, u) : A ∈ M, u ∈ [L]}.

First we claim that P(t) holds in expectation:

**Claim A.6:** For every Meridian node B and every step t: (a) E[F^t_B] < β/2 and (b) E[G^t_B/k'] < β/2.

Suppose property P(t-1) holds. Let's bound the load on some fixed node B. Note that F^t_B = Σ_{all pairs (A,u)} F_{A,B}(t-1, u) is a sum of independent random variables, each in [0, β/L]. Applying Claim A.9b with μ = β/2, we see that Pr[F^t_B > β] < (e/4)^(β/2) < δ/(2N·log_α(R)). Similarly, G^t_B = Σ_{(A,u)} G_{A,B}(t, u) is a sum of independent random variables, each in [0, β/L], so by Claim A.9b we can upper-bound Pr[G^t_B/k' > β]. By the Union Bound property P(t) holds with the total failure probability at most δ. This completes the proof of Claim A.5.

It remains to prove Claim A.6. Let S* be the set of queries q ∈ Q such that B is a nearest neighbor of the target T(q).

**Claim A.7:** |S*| < O(1)·log(N/δ).

*Proof:* Choose target T ∈ T(S*) such that d_{B,T} is maximal. Let d = d_{B,T}. Then B_T(d/2) ∩ {T} = ∅ for any T, so by Claim A.4 |X_T(d/2)| < O(log(N/δ)). Note that S* ⊆ B_T(2d) ⊆ X_T(2d) and |X_T(2d)| < (2/ε)^D·|X_T(d/2)| < O(log(N/δ)). Claim follows if we take small enough ε > 0. ∎

Let r* be the smallest r such that B_B(r) has cardinality at least twice the k' from Claim A.4. Let S_0 = Q_B(r*·β·α^i). Let S ⊆ Q be the set of queries that get forwarded to B on step t; recall that F^t_B = |S|.

**Claim A.8:** For any query q ∈ Q\(S* ∪ S_0) and T = T(q) we have Pr[q ∈ S] < O(1)/|B_B(d_{B,T})|.

*Proof:* Let d = d_{B,T} and suppose query q is currently at node A. Since q ∉ S* this query gets forwarded to some node C ∈ B_T(d_{A,T}/2), so if d < d_{A,T}/2 then clearly q ∉ S. Assume d ≥ d_{A,T}/2. Since B_B(d) ⊆ B_T(2d), by Claim A.4 we have |B_B(d)| ≤ |B_T(2d)| ≤ 2γ·|X_T(2d)| ≤ 2γ·2^D·|X_T(d)| ≤ 4·2^D·|B_T(d)|, Pr[q ∈ S] = 1/|B_T(d_{A,T}/2)| ≤ 1/|B_T(d)|, which is at most 4·2^D/|B_B(d)|, as required. ∎

Now for S_1 = S_0 ∪ S* and r = r*·β·α^i·γ^i:

E[|S\S_1|] ≤ |S_0|·Pr[q ∈ S | q ∈ S_0] < O(1)·|S_0|/|S_0| < O(1), E[F^t_B] = E[|S|] < |S*| + |S_0| + Σ_{i} O(1) < O(1)·log(N/δ) < β/2.

This completes the proof of Claim A.6a. For Claim A.6b, let S be the set of queries that cause a t-step request to B. Suppose a t-step query q is at node A; let T = T(q) and d = d_{B,T}. Node B receives a t-step request due to q only if d_{A,B} < 2d, so let's assume it is the case. Then d_{B,T} < d + d_{A,B} < 3d, so B_B(d_{B,T}) ⊆ B_B(d + d_{A,B}) ⊆ B_B(3d), |B_B(d_{B,T})| ≤ |B_B(3d)| ≤ 3·(2^D)·|B_B(d)|, Pr[B ∈ S] = 1/|B_B(2d)| ≤ 1/|B_B(d)|, as long as |B_B(d_{B,T})| is at least twice as large as the k' from Claim A.4. The rest of the proof of Claim A.6b is similar to that of Claim A.6a. This completes the proof of Claim A.6 and Thm. 4.4b.

### A.5 Chernoff Bounds

Essentially, Chernoff Bounds say that with high probability the sum of many bounded independent random variables is close to its expectation. Here for the sake of completeness we write out the standard Chernoff Bounds and some easy applications thereof that we use in the above proofs.

**Lemma A.9 (Chernoff Bounds):** Let X be the sum of independent random variables X_i ∈ [0, b], for some b > 0. Let ε ∈ (0, 1) and c > 1. Then:

(a) Pr[X ≤ (1-ε)·μ] < e^(-ε^2·μ/(2b)), for any μ ≤ E[X].

(b) Pr[X > c·μ] < (e^(c-1)/c^c)^(μ/b), for any μ ≥ E[X].

**Claim A.10:** Suppose ring R_{A,i} is well-formed and has cardinality k. Fix a subset S ⊆ B_{A,i} and let μ = k·|S|/|B_{A,i}|. Then with failure probability at most e^(-μ·(1-1/e)^2/2) some node from R_{A,i} lands in S.

*Proof:* Denote the desired event by E. The distribution of R_{A,i} is that of the following process P: pick nodes from B_{A,i} independently and uniformly at random, until we gather k_{A,i} distinct nodes. For simplicity consider a slightly modified process P': pick k_{A,i} nodes from B_{A,i} independently and uniformly at random, possibly with repetitions. Obviously, P' is doing exactly the same as P, except P might stop later and, accordingly, choose some more nodes. Therefore Pr_P[E] ≥ Pr_P'[E].

Let's analyze process P'. Let X_j be a 0-1 random variable that is equal to 1 if and only if the j-th chosen node lands in B_B(r). Then Pr[X_j = 1] = |S|/|B_{A,i}|, so μ = E[Σ X_j]. The claim follows from Lemma A.9a with b = 1 and 1-ε = 1/μ. ∎

**Claim A.11:** Consider two sets S' ⊆ S and suppose m nodes are chosen independently and uniformly at random from S; say X of these m nodes land in S'. Let μ = m·|S'|/|S|. Then:

(a) Pr[X ≤ μ/2] < e^(-μ/8),

(b) Pr[X > c·μ] < (e^(c-1)/c^c)^μ for any c > 1,

(c) Pr[X > 2μ] < (e/4)^μ.

*Proof:* Let X_j be a 0-1 random variable that is equal to 1 if and only if the j-th chosen node lands in S'. Then Pr[X_j = 1] = |S'|/|S|, so X = Σ_{j=1}^m X_j and μ = E[X].

For part (a), use Lemma A.9a with b = 1 and ε = 1/2. Parts (bc) follow from Lemma A.9b with b = 1 and c > 1; let μ = c in part (b), and let μ = 2 in part (c). ∎
```