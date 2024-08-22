#include <qmainwindow.h>
#include <qwidget.h>

class FrmMain : public QMainWindow
{ 
        Q_OBJECT 
public: 
        FrmMain(QWidget* parent=0, const char* name=0, WFlags fl=0); 

public slots:
 
void    cmdFileQuit();

protected:
	void	timerEvent(QTimerEvent *);
};

class QtVDU : public QWidget
{
	Q_OBJECT
public:
	QtVDU(QWidget *parent=0, const char *name=0);
	QSizePolicy sizePolicy() const;
protected:
	void paintEvent(QPaintEvent *);
	void keyPressEvent(QKeyEvent *);
	void keyReleaseEvent(QKeyEvent *);
	void mouseMoveEvent(QMouseEvent *);
};
