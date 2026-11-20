import 'dart:async';

import 'package:flutter/material.dart';

import '../widgets/compact_controls.dart';
import '../widgets/reading_pane.dart';
import 'package:flutter/services.dart';

import '../localization/litch_strings.dart';
import '../localization/presentation_strings.dart';
import '../models/support_report.dart';
import '../services/litch_ble_service.dart';
import '../services/support_email_launcher.dart';
import '../services/support_report_service.dart';

typedef SupportDraftLoader = Future<SupportReportDraft> Function();

class SupportReportScreen extends StatefulWidget {
  const SupportReportScreen({
    super.key,
    this.bleService,
    this.boardId,
    this.loadDraft,
    this.emailLauncher,
  });

  final LitchBleService? bleService;
  final String? boardId;
  final SupportDraftLoader? loadDraft;
  final SupportEmailLauncher? emailLauncher;

  static Future<void> open(
    BuildContext context, {
    LitchBleService? bleService,
    String? boardId,
  }) async {
    await Navigator.of(context).push<void>(
      MaterialPageRoute<void>(
        builder: (BuildContext context) =>
            SupportReportScreen(bleService: bleService, boardId: boardId),
      ),
    );
  }

  @override
  State<SupportReportScreen> createState() => _SupportReportScreenState();
}

class _SupportReportScreenState extends State<SupportReportScreen> {
  final TextEditingController _subjectController = TextEditingController();
  final TextEditingController _bodyController = TextEditingController();
  SupportReportDraft? _draft;
  SupportReportService? _reportService;
  Object? _loadError;
  bool _openingEmail = false;
  String? _generatedSubject;
  String? _generatedBody;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _localizeUneditedDefaults();
  }

  void _localizeUneditedDefaults() {
    final draft = _draft;
    if (draft == null || draft.diagnostics == null) return;
    final strings = litchStrings(context);
    final subject = draft.healthContext
        ? strings.supportHealthSubject
        : strings.supportSubject;
    final body = localizedSupportBody(strings, draft);
    // Locale changes must never overwrite answers or a manually edited subject.
    if (_subjectController.text == (_generatedSubject ?? draft.subject)) {
      _subjectController.text = subject;
      _generatedSubject = subject;
    }
    if (_bodyController.text == (_generatedBody ?? draft.body)) {
      _bodyController.text = body;
      _generatedBody = body;
    }
  }

  @override
  void initState() {
    super.initState();
    unawaited(_load());
  }

  Future<void> _load() async {
    try {
      if (widget.loadDraft == null) {
        _reportService = SupportReportService(
          bleService: widget.bleService,
          boardId: widget.boardId,
        );
      }
      final SupportDraftLoader loader =
          widget.loadDraft ?? _reportService!.createDraft;
      final SupportReportDraft draft = await loader();
      if (!mounted) {
        return;
      }
      _subjectController.text = draft.subject;
      _bodyController.text = draft.body;
      setState(() {
        _draft = draft;
      });
      _localizeUneditedDefaults();
    } catch (error) {
      if (!mounted) {
        return;
      }
      setState(() {
        _loadError = error;
      });
    }
  }

  @override
  void dispose() {
    _reportService?.cancel();
    _subjectController.dispose();
    _bodyController.dispose();
    super.dispose();
  }

  Future<void> _copyReport({bool emailUnavailable = false}) async {
    final SupportReportDraft? draft = _draft;
    if (draft == null) {
      return;
    }
    final String copied = <String>[
      'To: ${draft.recipient}',
      'Subject: ${_subjectController.text}',
      '',
      _bodyController.text,
    ].join('\n');
    await Clipboard.setData(ClipboardData(text: copied));
    if (!mounted) {
      return;
    }
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(
          emailUnavailable
              ? litchStrings(context).supportEmailUnavailable
              : litchStrings(context).supportCopied,
        ),
      ),
    );
  }

  Future<void> _openEmail() async {
    final SupportReportDraft? draft = _draft;
    if (draft == null || _openingEmail) {
      return;
    }
    setState(() {
      _openingEmail = true;
    });
    final SupportEmailLaunchResult result =
        await (widget.emailLauncher ?? SupportEmailLauncher.shared).open(
          recipient: draft.recipient,
          subject: _subjectController.text,
          body: _bodyController.text,
        );
    if (!mounted) {
      return;
    }
    setState(() {
      _openingEmail = false;
    });
    switch (result) {
      case SupportEmailLaunchResult.opened:
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(litchStrings(context).supportEmailOpened)),
        );
        break;
      case SupportEmailLaunchResult.cooldown:
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(litchStrings(context).supportEmailCooldown)),
        );
        break;
      case SupportEmailLaunchResult.unavailable:
      case SupportEmailLaunchResult.failed:
        await _copyReport(emailUnavailable: true);
        break;
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: LitchCompactAppBar(
        child: AppBar(title: Text(litchStrings(context).contactSupport)),
      ),
      body: SafeArea(child: LitchReadingPane(child: _buildBody(context))),
    );
  }

  Widget _buildBody(BuildContext context) {
    if (_loadError != null) {
      return Center(
        child: Padding(
          padding: EdgeInsets.all(24),
          child: Text(
            litchStrings(context).supportLoadFailed,
            textAlign: TextAlign.center,
          ),
        ),
      );
    }
    final SupportReportDraft? draft = _draft;
    if (draft == null) {
      return Center(
        child: Semantics(
          label: litchStrings(context).supportPreparing,
          child: const CircularProgressIndicator(),
        ),
      );
    }

    return ListView(
      key: const ValueKey<String>('support-report-list'),
      padding: const EdgeInsets.fromLTRB(16, 16, 16, 28),
      children: <Widget>[
        Text(
          litchStrings(context).recipientLabel(draft.recipient),
          style: Theme.of(context).textTheme.titleMedium,
        ),
        const SizedBox(height: 8),
        Text(litchStrings(context).supportConsent),
        const SizedBox(height: 16),
        TextField(
          key: const ValueKey<String>('support-subject'),
          controller: _subjectController,
          maxLength: SupportReportDraft.maxSubjectCharacters,
          decoration: InputDecoration(
            labelText: litchStrings(context).subjectLabel,
            border: OutlineInputBorder(),
          ),
        ),
        const SizedBox(height: 8),
        TextField(
          key: const ValueKey<String>('support-report-body'),
          controller: _bodyController,
          minLines: 18,
          maxLines: null,
          maxLength: SupportReportDraft.maxBodyCharacters,
          decoration: InputDecoration(
            labelText: litchStrings(context).supportReportLabel,
            alignLabelWithHint: true,
            border: OutlineInputBorder(),
          ),
        ),
        const SizedBox(height: 8),
        Text(litchStrings(context).supportPrivacy),
        const SizedBox(height: 16),
        Wrap(
          spacing: 12,
          runSpacing: 12,
          children: <Widget>[
            FilledButton.icon(
              key: const ValueKey<String>('open-support-email'),
              onPressed: _openingEmail ? null : _openEmail,
              icon: _openingEmail
                  ? const SizedBox.square(
                      dimension: 18,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    )
                  : const Icon(Icons.email_outlined),
              label: Text(litchStrings(context).openEmailDraft),
            ),
            OutlinedButton.icon(
              key: const ValueKey<String>('copy-support-report'),
              onPressed: _openingEmail ? null : () => _copyReport(),
              icon: const Icon(Icons.copy_outlined),
              label: Text(litchStrings(context).copyReport),
            ),
          ],
        ),
      ],
    );
  }
}
