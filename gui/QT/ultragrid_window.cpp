#include <QMessageBox>

#include "ultragrid_window.hpp"

UltragridWindow::UltragridWindow(QWidget *parent): QMainWindow(parent){
	ui.setupUi(this);
	ui.terminal->setVisible(false);

	QStringList args = QCoreApplication::arguments();
	int index = args.indexOf("--with-uv");
	if(index != -1 && args.size() >= index + 1) {
		//found
		ultragridExecutable = args.at(index + 1);
	} else {
		ultragridExecutable = "uv";
	}

	connect(ui.actionAbout_UltraGrid, SIGNAL(triggered()), this, SLOT(about()));
	connect(ui.startButton, SIGNAL(clicked()), this, SLOT(start()));
	connect(&process, SIGNAL(readyReadStandardOutput()), this, SLOT(outputAvailable()));
	connect(&process, SIGNAL(readyReadStandardError()), this, SLOT(outputAvailable()));
	connect(&process, SIGNAL(stateChanged(QProcess::ProcessState)), this, SLOT(processStateChanged(QProcess::ProcessState)));

	connect(ui.networkDestinationEdit, SIGNAL(textEdited(const QString &)), this, SLOT(setArgs()));

	connect(ui.arguments, SIGNAL(textChanged(const QString &)), this, SLOT(editArgs(const QString &)));
	connect(ui.editCheckBox, SIGNAL(toggled(bool)), this, SLOT(setArgs()));
	connect(ui.actionRefresh, SIGNAL(triggered()), this, SLOT(queryOpts()));
	connect(ui.actionAdvanced, SIGNAL(toggled(bool)), this, SLOT(setAdvanced(bool)));
	connect(ui.actionShow_Terminal, SIGNAL(triggered()), this, SLOT(showLog()));
	connect(ui.previewCheckBox, SIGNAL(toggled(bool)), this, SLOT(enablePreview(bool)));

	sourceOption = new VideoSourceOption(&ui, ultragridExecutable);
	opts.emplace_back(sourceOption);

	displayOption = new VideoDisplayOption(&ui, ultragridExecutable);
	opts.emplace_back(displayOption);

	opts.emplace_back(new VideoCompressOption(&ui,
				ultragridExecutable));

	opts.emplace_back(new AudioSourceOption(&ui,
				sourceOption,
				ultragridExecutable));

	opts.emplace_back(new AudioPlaybackOption(&ui,
				displayOption,
				ultragridExecutable));

	opts.emplace_back(new AudioCompressOption(&ui,
				ultragridExecutable));

	opts.emplace_back(new FecOption(&ui));
	opts.emplace_back(new ParamOption(&ui));

	for(auto &opt : opts){
		connect(opt.get(), SIGNAL(changed()), this, SLOT(setArgs()));
	}

	connect(sourceOption, SIGNAL(changed()), this, SLOT(startPreview()));

	queryOpts();

	checkPreview();

	startPreview();
}

void UltragridWindow::checkPreview(){
	QStringList out;

	QProcess process;

	QString command = ultragridExecutable;

	command += " -d help";

	process.start(command);

	process.waitForFinished();
	QByteArray output = process.readAllStandardOutput();
	QList<QByteArray> lines = output.split('\n');

	foreach ( const QByteArray &line, lines ) {
		if(line.size() > 0 && QChar(line[0]).isSpace()) {
			QString opt = QString(line).trimmed();
			if(opt != "none"
					&& !opt.startsWith("--")
					&& !opt.contains("unavailable"))
				out.append(QString(line).trimmed());
		}
	}

	if(!out.contains("multiplier") || !out.contains("preview")){
		ui.previewCheckBox->setChecked(false);
		ui.previewCheckBox->setEnabled(false);

		QMessageBox warningBox(this);
		warningBox.setWindowTitle("Preview disabled");
		warningBox.setText("Preview is disabled, because UltraGrid was compiled" 
				" without preview and multiplier displays."
				" Please build UltraGrid configured with the --enable-qt flag"
				" to enable preview.");
		warningBox.setStandardButtons(QMessageBox::Ok);
		warningBox.setIcon(QMessageBox::Warning);
		warningBox.exec();
	}
}

void UltragridWindow::about(){
	QMessageBox aboutBox(this);
	aboutBox.setWindowTitle("About UltraGrid");
	aboutBox.setTextFormat(Qt::RichText);
	aboutBox.setText( "UltraGrid from CESNET is a software "
		"implementation of high-quality low-latency video and audio transmissions using commodity PC and Mac hardware.<br><br>"
		"More information can be found at <a href='http://www.ultragrid.cz'>http://www.ultragrid.cz</a><br><br>"
		"Please read documents distributed with the product to find out current and former developers."
		);
	aboutBox.setStandardButtons(QMessageBox::Ok);
	aboutBox.exec();
}

void UltragridWindow::outputAvailable(){
	//ui.terminal->append(process.readAll());
	
	QString str = process.readAll();
#if 1
	ui.terminal->moveCursor(QTextCursor::End);
	ui.terminal->insertPlainText(str);
	ui.terminal->moveCursor(QTextCursor::End);
#endif
	log.write(str);
}

void UltragridWindow::start(){
	if(process.pid() > 0){
		process.terminate();
		if(!process.waitForFinished(1000))
			process.kill();
		return;
	}

	stopPreview();

	QString command(ultragridExecutable);

	command += " ";
	command += launchArgs;
	process.setProcessChannelMode(QProcess::MergedChannels);
	log.write("Command: " + command + "\n\n");
	process.start(command);
}

void UltragridWindow::startPreview(){
	if(!ui.previewCheckBox->isEnabled()
			|| process.state() != QProcess::NotRunning
			|| !ui.previewCheckBox->isChecked()
			){
		return;
	}

	if(previewProcess.state() != QProcess::NotRunning)
		stopPreview();

	QString command(ultragridExecutable);
	command += " ";
	command += sourceOption->getLaunchParam();
	command += "-d preview ";
	if(sourceOption->getCurrentValue() != "none"){
		//We prevent video from network overriding local sources
		//by listening on port 0
		command += "-P 0 ";
	}

	previewProcess.start(command);
}

void UltragridWindow::stopPreview(){
	previewProcess.terminate();
	/* The shared preview memory must be released before a new one
	 * can be created. Here we wait 0.5s to allow the preview process
	 * exit gracefully. If it is still running after that we kill it */
	if(!previewProcess.waitForFinished(500))
		previewProcess.kill();
}

void UltragridWindow::editArgs(const QString &text){
	launchArgs = text;
}

void UltragridWindow::setArgs(){
	launchArgs = "";

	for(auto &opt : opts){
		launchArgs += opt->getLaunchParam();
	}

	launchArgs += ui.networkDestinationEdit->text();

	ui.arguments->setText(launchArgs);
}

void UltragridWindow::queryOpts(){
	for(auto &opt : opts){
		opt->queryAvailOpts();
	}

	setArgs();
}

void UltragridWindow::closeEvent(QCloseEvent *){
	log.close();
}

void UltragridWindow::setAdvanced(bool adv){
	for(auto &opt : opts){
		opt->setAdvanced(adv);
	}
	queryOpts();
}

void UltragridWindow::showLog(){
	log.show();
	log.raise();
}

void UltragridWindow::setStartBtnText(QProcess::ProcessState s){
	if(s == QProcess::Running){
		ui.startButton->setText("Stop");
	} else {
		ui.startButton->setText("Start");
	}
}

void UltragridWindow::enablePreview(bool enable){
	if(enable)
		startPreview();
	else
		stopPreview();
}

void UltragridWindow::processStateChanged(QProcess::ProcessState s){
	setStartBtnText(s);

	if(s == QProcess::NotRunning){
		startPreview();
	}
}
