import CoreBluetooth

struct ServicesWithCharacteristicsDiscoveryTaskController: PeripheralTaskController {

    typealias TaskSpec = ServicesWithCharacteristicsDiscoveryTaskSpec

    private let task: SubjectTask

    init(_ task: SubjectTask) {
        self.task = task
    }

    func start(peripheral: CBPeripheral) -> SubjectTask {
        guard case .pending = task.state
        else {
            return task
                .with(state: task.state.finished([
                    improperHandling(currentState: task.state)
                ]))
        }

        if task.params.servicesWithCharacteristicsToDiscover.isEmpty {
            return task.with(state: task.state.finished([]))
        } else {
            task.params.diagnostics?.record("services_requested", peripheralId: peripheral.identifier,
                fields: ["all_services": task.params.servicesWithCharacteristicsToDiscover.services == nil,
                         "service_count": task.params.servicesWithCharacteristicsToDiscover.services?.count ?? 0])
            peripheral.discoverServices(task.params.servicesWithCharacteristicsToDiscover.services)

            return task.with(state: task.state.processing(.discoveringServices))
        }
    }

    func cancel(error: Error) -> SubjectTask {
        task.params.diagnostics?.record("discovery_cancelled", peripheralId: task.key,
            fields: ["error_code": (error as NSError).code])
        return task
            .with(state: task.state.finished([error]))
    }

    func handleServicesDiscovery(peripheral: CBPeripheral, error: Error?) -> SubjectTask {
        guard case .processing(since: _, .discoveringServices) = task.state
        else {
            return task
                .with(state: task.state.finished([
                    improperHandling(currentState: task.state)
                ]))
        }

        if let error = error {
            return task
                .with(state: task.state.finished([error]))
        } else {
            let scheduledCharacteristicsDiscoveryCount = forEachService(
                    in: task.params.servicesWithCharacteristicsToDiscover,
                    with: peripheral.services
                ) { service, characteristicsToDiscover in
                    task.params.diagnostics?.record("characteristics_requested", peripheralId: peripheral.identifier,
                        fields: ["service_slot": peripheral.services?.firstIndex(where: { $0 === service }) ?? -1,
                                 "all_characteristics": characteristicsToDiscover.characteristics == nil,
                                 "characteristic_count": characteristicsToDiscover.characteristics?.count ?? 0])
                    peripheral.discoverCharacteristics(characteristicsToDiscover.characteristics, for: service)
                }

            return task
                .with(state: scheduledCharacteristicsDiscoveryCount > 0
                    ? task.state.processing(.discoveringCharacteristics(
                        servicesLeft: scheduledCharacteristicsDiscoveryCount,
                        errors: []
                    ))
                    : task.state.finished([])
                )
        }
    }

    func handleCharacteristicsDiscovery(
        service: CBService,
        error: Error?
    ) -> SubjectTask {
        guard
            case .processing(since: _, .discoveringCharacteristics(let servicesLeft, let errors)) = task.state,
            task.params.servicesWithCharacteristicsToDiscover.services.map({ $0.contains(service.uuid) }) != false
        else {
            return task
                .with(state: task.state.finished([improperHandling(currentState: task.state)]))
        }

        let accumulatedErrors = errors + [error].compactMap { $0 }
        return task
            .with(state: servicesLeft > 1
                ? task.state.processing(.discoveringCharacteristics(
                    servicesLeft: servicesLeft - 1,
                    errors: accumulatedErrors
                ))
                : task.state.finished(accumulatedErrors)
            )
    }

    private func forEachService(
        in servicesWithCharacteristicsToDiscover: ServicesWithCharacteristicsToDiscover,
        with knownServices: [CBService]?,
        _ body: (CBService, CharacteristicsToDiscover) -> Void
    ) -> Int {
        let items: [(CBService, CharacteristicsToDiscover)]?

        switch servicesWithCharacteristicsToDiscover {
        case .all:
            items = knownServices?
                .map { service in (service, CharacteristicsToDiscover.all) }
        case .some(let servicesWithCharacteristicsToDiscover):
            items = servicesWithCharacteristicsToDiscover
                .compactMap { item in
                    knownServices?
                        .filter({ $0.uuid == item.key })
                        .map { service in (service, item.value) }
                }.flatMap { $0 }
        }

        items?.forEach(body)

        return items?.count ?? 0
    }
}
